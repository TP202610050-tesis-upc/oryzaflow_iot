# OryzaFlow — Esquema de Telemetría del Edge (contrato congelado)

> **Estado:** CONGELADO v1.0 — 2026-06-21.
> Este documento es la **fuente de verdad** de lo que el edge (ESP32) publica a
> ThingsBoard y, por tanto, de lo que el backend (Spring Boot) ingiere por MQTT y
> persiste. Cualquier cambio de campo/tipo/unidad rompe el contrato edge↔backend↔móvil
> y debe versionarse aquí primero.
>
> Fundamentos: `docs/CONTEXTO_PROYECTO.md` §7 (modelos), §9 (rangos), §16 (sensores);
> `docs/CONTRATO_API_MOBILE.md` §0 (ingesta MQTT, reparto edge↔backend, crudo vs
> derivado), §4 (`SensorReadingModel`, `NodeModel`, `Esp32ConfigModel`).

---

## 0. Principio de diseño: JSON canónico + transporte conmutable

El **esquema JSON canónico (§2)** es el contrato. El medio físico es intercambiable
mediante un **interruptor de transporte** en el firmware:

```
                                   ┌──────────────────────────────────────────┐
   ESP32 (modelo de secado)  ──►   │  TRANSPORTE (switch en firmware)          │
   produce el JSON canónico        │                                           │
                                   │  modo WIFI:                               │
                                   │    JSON ──MQTT/TLS──► ThingsBoard         │
                                   │                                           │
                                   │  modo LORAWAN:                            │
                                   │    JSON ──encode()──► bytes ──LoRa──►      │
                                   │      Gateway ─► NS ─► decode() ─► JSON ─►  │
                                   │                              ThingsBoard  │
                                   └──────────────────────────────────────────┘
                                                       │
                                                       ▼
                                        Backend (suscriptor MQTT) ─► MySQL
```

- **El backend ve siempre el mismo JSON**, venga por WiFi o por LoRaWAN.
- El `encode()` (edge) y el `decode()` (NS/ThingsBoard) son **inversos exactos** y
  comparten la especificación binaria de §5. Probarlos como par es obligatorio
  (round-trip: `decode(encode(x)) == x` dentro de la resolución de cuantización).
- Modo de transporte por defecto en prototipo de tesis: **WIFI** (más simple,
  coherente con `Esp32ConfigModel.ssid/ipAddress/puerto/wifiActivo`). **LORAWAN**
  queda listo como ruta de producción (respeta duty cycle, §4).

---

## 1. Qué es crudo y qué es derivado

| Categoría | Campos | Origen |
|---|---|---|
| **Crudo (sensado)** | `t`, `rh`, `rain` | SHT31 (T°, HR) + YL-83 (lluvia digital) |
| **Derivado tipo A** (solo ambiente) | `me` | Henderson modificado — el edge lo calcula **siempre** (autónomo) |
| **Derivado tipo B** (contexto de lote) | `m`, `mr`, `dr`, `te` | Page — **requiere** parámetros aprovisionados por el backend (§3) |

> **Clave del modelo:** el ESP32 **no tiene sensor de humedad de grano**. `m = M(t)`
> se **modela** con Page a partir del *tiempo transcurrido del lote* y de `me` actual:
>
> ```
> Me  = ( −ln(1 − HR) / ( A·(T + C) ) )^(1/B)        (Henderson; A=2.448e-5, B=2.334, C=35.703)
> MR  = exp( −k · tⁿ )                                (Page; t = horas desde inicio del lote)
> M(t)= Me + (M₀ − Me)·MR                              (despeje de MR = (M−Me)/(M₀−Me))
> DR  = (M_prev − M(t)) / Δt_horas       [%/h]        (entre dos publicaciones)
> te  = ( −ln(MR_obj) / k )^(1/n) − t     [h]         (tiempo restante; MR_obj=(M_target−Me)/(M₀−Me))
> ```
>
> Por eso el edge necesita del backend `M₀, M_target, k, n` **y** el instante de
> inicio del lote (§3). Sin lote activo, el edge solo emite los campos tipo A/crudos.

**Conversiones de unidad dentro del edge (no salen en telemetría):**
- Henderson usa `HR` como **fracción** (`rh/100`) y `T` en **°C**.
- Arrhenius (`D_eff`, informativo, no se publica) usa `T` en **Kelvin** (`T+273.15`).
- Page: `t` en **horas**; `k` y `n` deben haberse ajustado **en horas** (el backend
  aprovisiona `k/n` consistentes con esta unidad — ver §3).

---

## 2. Esquema JSON canónico (telemetría)

Tópico ThingsBoard (modo WiFi): `v1/devices/me/telemetry`.

| Clave | Nombre dominio | Tipo | Unidad | Cat. | Rango esperado (§9) | Notas |
|---|---|---|---|---|---|---|
| `ts`   | timestamp        | int  | ms epoch UTC | meta   | —          | Opcional; si se omite, ThingsBoard sella al recibir. |
| `t`    | Temperatura      | num  | °C    | crudo   | 35–45      | SHT31, **con offset** aplicado. `>45` ⇒ riesgo fisuras. |
| `rh`   | Humedad relativa | num  | %     | crudo   | 40–70      | SHT31, **con offset** aplicado. |
| `rain` | Lluvia           | bool | —     | crudo   | false      | YL-83 digital. `true` ⇒ estado "cobertura". |
| `me`   | Humedad equilibrio (Me) | num | % grano | deriv A | —     | Henderson; recalculado **cada ciclo**. |
| `m`    | Humedad de grano (M(t)) | num | % grano | deriv B | 18–26→target | Modelado (Page). Solo si lote activo. |
| `mr`   | Razón de humedad (MR)   | num | 0–1   | deriv B | 0–1        | `≤0.05` ⇒ proceso concluido. Solo si lote activo. |
| `dr`   | Tasa de secado (DR)     | num | %/h   | deriv B | 0.5–2.5    | `<0.5` lento; `>2.5` riesgo. Solo si lote activo. |
| `te`   | Tiempo restante estimado| num | h     | deriv B | —          | **Opcional pero recomendado** (app HU-curva). Solo si lote activo. |
| `lot`  | Lote activo en el edge  | bool | —     | meta   | —          | `false` ⇒ campos tipo B ausentes (Type A only). |

**Reglas de presencia:**
- Campos **siempre presentes:** `t`, `rh`, `rain`, `me`, `lot`.
- Campos tipo B (`m`, `mr`, `dr`, `te`) **presentes solo si `lot=true`** (hay lote
  aprovisionado y `t_transcurrido ≥ 0`). Nunca se envían como `null`: se **omiten**.
- `dr` se omite además en la **primera** publicación del lote (no hay `M_prev`).

**Tipos:** todos los numéricos son `double`/número JSON. Sin unidades en el valor.

### 2.1 Ejemplo — lote activo (camino normal)

```json
{
  "ts": 1718900000000,
  "t": 41.2,
  "rh": 58.3,
  "rain": false,
  "me": 12.1,
  "m": 16.4,
  "mr": 0.42,
  "dr": 1.8,
  "te": 9.5,
  "lot": true
}
```

### 2.2 Ejemplo — sin lote activo (solo ambiente, tipo A)

```json
{
  "ts": 1718900000000,
  "t": 39.7,
  "rh": 61.0,
  "rain": false,
  "me": 12.6,
  "lot": false
}
```

### 2.3 Ejemplo — lluvia (dispara cobertura aguas abajo)

```json
{ "ts": 1718900300000, "t": 28.4, "rh": 88.0, "rain": true, "me": 22.9, "lot": true,
  "m": 16.3, "mr": 0.41, "dr": 0.1, "te": 40.0 }
```

> El edge **no** decide alertas; solo publica. Las reglas de §9/§10 del contexto las
> evalúa el motor de alertas del backend (BC5) sobre esta telemetría.

---

## 3. Aprovisionamiento del backend → edge (shared attributes / RPC)

Al **iniciar un lote** sobre el nodo, el backend empuja estos *shared attributes* a
ThingsBoard (tópico `v1/devices/me/attributes` para el edge en modo WiFi; vía RPC o
downlink en LoRaWAN). Sin esto el edge **no** puede calcular tipo B.

```json
{
  "loteId": "LOTE-2026-0042",
  "loteInicioTs": 1718900000000,
  "m0": 22.5,
  "mTarget": 13.0,
  "perfil": "caseritaNormal",
  "k": 0.1200,
  "n": 1.0500,
  "intervaloEnvioSegundos": 300,
  "offsetT": -0.30,
  "offsetRh": 1.20,
  "offsetM": 0.00,
  "tMax": 45.0
}
```

| Atributo | Tipo | Unidad | Uso en el edge |
|---|---|---|---|
| `loteId` | string | — | Trazabilidad; se refleja en client-attribute (§6). |
| `loteInicioTs` | int | ms epoch UTC | Ancla para `t = (now − loteInicioTs)` en **horas**. |
| `m0` | num | % grano | `M₀` en Page / MR_obj. |
| `mTarget` | num | % grano | `M_target` para `MR_obj` y `te`. |
| `perfil` | string | — | Etiqueta comercial (§8 contexto); informativa. |
| `k` | num | (1/hⁿ) | Constante Page, **ajustada en horas**. |
| `n` | num | — | Exponente Page. |
| `intervaloEnvioSegundos` | int | s | Periodo de **publicación** (§4). `Esp32Config`. |
| `offsetT` | num | °C | Calibración SHT31 T (HU-44). `t = t_raw + offsetT`. |
| `offsetRh` | num | % | Calibración SHT31 HR. `rh = rh_raw + offsetRh`. |
| `offsetM` | num | % grano | Corrección del modelo `m` (def. 0). |
| `tMax` | num | °C | Umbral de temperatura del lote/variedad (informativo). |

**Fin de lote:** al completar/cancelar, el backend empuja `{"loteId": null}` (o
`lot=false` por RPC). El edge vuelve a modo tipo A (sigue publicando `t/rh/rain/me`).

> `offsetT/offsetRh/offsetM` también pueden vivir como *shared attributes*
> persistentes del nodo (calibración, no del lote). Mapean a
> `Esp32ConfigModel.offsetTemperatura/offsetHumedad/offsetHumedadGrano`.

---

## 4. Frecuencia: muestreo vs publicación

| Acción | Periodo por defecto | Configurable | Razón |
|---|---|---|---|
| **Muestreo** sensores (suaviza ruido) | 60 s | const firmware | Promedio móvil; SHT31 es estable. |
| **Publicación** telemetría | 300 s (5 min) | `intervaloEnvioSegundos` | El secado es lento (DR en %/h); 5 min basta. |

- `DR` se calcula entre **publicaciones consecutivas**: `Δt = intervaloEnvioSegundos/3600` h.
- **Duty cycle LoRaWAN:** un payload de ~7–15 bytes a SF7/EU868 ≈ 50 ms de airtime;
  al 1% de duty cycle se permite ~1 envío / 5 s. Publicar cada 5 min deja **enorme**
  margen. Si se sube SF (alcance), recalcular airtime antes de bajar el intervalo.
- **Watchdog de desconexión (BC5):** el backend marca "Desconexión" si no llega
  telemetría por > N·`intervaloEnvioSegundos` (N≈3). El edge debe publicar aunque
  no haya lote (tipo A) para mantener el latido.

---

## 5. Codec binario (modo LoRaWAN)

Especificación del payload para `encode()` (edge) / `decode()` (NS/ThingsBoard).
**Big-endian (MSB primero)**, sin padding. Reconstruye exactamente el JSON de §2.

### 5.1 Disposición de bytes

| Offset | Campo | Tipo bin | Escala (valor entero = …) | Presencia |
|---|---|---|---|---|
| 0 | `flags` | uint8 | bitfield (§5.2) | siempre |
| 1–2 | `t`  | int16  | `round(t × 100)`   | siempre |
| 3–4 | `rh` | uint16 | `round(rh × 100)`  | siempre |
| 5–6 | `me` | uint16 | `round(me × 100)`  | siempre |
| 7–8 | `m`  | uint16 | `round(m × 100)`   | solo si `lot` |
| 9–10 | `mr` | uint16 | `round(mr × 10000)`| solo si `lot` |
| 11–12 | `dr` | int16 | `round(dr × 100)`  | solo si `lot` y existe `M_prev` |
| 13–14 | `te` | uint16 | `round(te × 100)`  | solo si `lot` y `te` presente |

Longitudes resultantes: **7 bytes** (sin lote) · **11 bytes** (lote, 1ª pub sin dr/te)
· **15 bytes** (lote completo con dr y te). El `decode()` usa `flags` + longitud para
saber qué campos vienen.

### 5.2 Byte de flags (offset 0)

| Bit | Nombre | Significado |
|---|---|---|
| 0 | `lot`     | 1 = lote activo (vienen campos tipo B). |
| 1 | `rain`    | 1 = lluvia (YL-83). |
| 2 | `hasDr`   | 1 = `dr` presente en payload. |
| 3 | `hasTe`   | 1 = `te` presente en payload. |
| 4 | `fault`   | 1 = fallo lectura SHT31 (`t`/`rh` no fiables). |
| 5–7 | reservado | 0. |

### 5.3 Decoder de referencia (JavaScript — TTN/ChirpStack/ThingsBoard)

```javascript
// decode(bytes) -> JSON canónico (§2). bytes: Array<int> | Buffer (big-endian)
function decode(bytes) {
  const u16 = (i) => (bytes[i] << 8) | bytes[i + 1];
  const s16 = (i) => { const v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; };

  const flags = bytes[0];
  const lot   = !!(flags & 0x01);
  const out = {
    t:    s16(1) / 100,
    rh:   u16(3) / 100,
    rain: !!(flags & 0x02),
    me:   u16(5) / 100,
    lot:  lot,
  };
  if (flags & 0x10) out.fault = true;

  let off = 7;
  if (lot) {
    out.m  = u16(off) / 100;  off += 2;
    out.mr = u16(off) / 10000; off += 2;
    if (flags & 0x04) { out.dr = s16(off) / 100; off += 2; }   // hasDr
    if (flags & 0x08) { out.te = u16(off) / 100; off += 2; }   // hasTe
  }
  return out;
}
```

> El `encode()` del firmware (MicroPython, `struct.pack('>...')`) es el inverso byte
> a byte de esto. La cuantización (×100 / ×10000) define la resolución: 0.01 en
> `t/rh/me/m/dr/te` y 0.0001 en `mr` — muy por encima de la precisión de los sensores
> y del modelo, así que el round-trip no pierde información útil.

---

## 6. Atributos que el edge reporta de vuelta (client-side attributes)

Para alimentar el estado vivo del nodo en el móvil (`NodeModel`, `Esp32ConfigModel`).
Tópico `v1/devices/me/attributes` (WiFi) / metadatos LoRaWAN.

```json
{
  "fwVersion": "1.0.0",
  "transport": "wifi",
  "loteId": "LOTE-2026-0042",
  "rssi": -67,
  "ip": "192.168.1.42",
  "intervaloEnvioSegundos": 300,
  "bootCount": 12
}
```

Estos son **atributos**, no telemetría: cambian poco y se consultan por estado, no
por serie temporal.

---

## 7. Mapeo edge → backend → móvil

El backend traduce las claves cortas a los nombres que el móvil consume. Referencia
para el suscriptor MQTT (BC4) y `SensorReadingModel`/`NodeModel` (§4 del contrato).

| Telemetría (edge) | `SensorReadingModel` | `NodeModel` (último valor) | Persistencia (`LECTURA`) |
|---|---|---|---|
| `ts` | `timestamp` | `ultimaActualizacion` | `timestamp` (UTC) |
| `t`  | `temperatura` | `temperatura` | `temperatura` |
| `rh` | `humedad` | `humedad` | `humedad` |
| `m`  | `humedadGrano` | `humedadGrano` | `humedad_grano` |
| `me` | — (derivado) | — | `humedad_equilibrio` |
| `mr` | — | — | `razon_humedad` |
| `dr` | — | — | `tasa_secado` |
| `te` | — | — | `tiempo_restante_h` |
| `rain` | — | (estado "cobertura") | `lluvia` |
| `lot` | — | `loteActivoId` (vía `loteId` attr) | — |

> `flujoAire` **NO existe** en este esquema (sin sensor ni cálculo). El backend debe
> dejar de exponerlo / el móvil lo eliminará (`CONTRATO_API_MOBILE.md` §0 y §7.1).
> Campos derivados sin columna en el móvil (`me/mr/dr/te`) igual se **persisten** en
> el backend porque alimentan los datasets de gráficas (HU-38 curva de secado,
> HU-39 temperatura) y KPIs (BC6).

---

## 8. Checklist de "esquema congelado" (lo que desbloquea el backend)

- [x] Claves, tipos, unidades y rangos de telemetría (§2).
- [x] Reglas de presencia tipo A vs tipo B (§2, §5.2).
- [x] Contrato de aprovisionamiento backend→edge (§3).
- [x] Frecuencia de publicación y latido de desconexión (§4).
- [x] Codec binario LoRaWAN + decoder de referencia (§5).
- [x] Mapeo a `SensorReadingModel`/`NodeModel` y columnas de `LECTURA` (§7).

**Siguiente en esta sesión IoT:** (2) configurar ThingsBoard (device, credenciales
MQTT/TLS, shared attributes, decoder LoRaWAN) y (3) firmware ESP32 (MicroPython:
leer SHT31+YL-83, aplicar offsets, calcular Henderson+Page+DR, conmutar transporte
WiFi/LoRaWAN, publicar este esquema).

# OryzaFlow — Esquema de Telemetría del Edge (contrato congelado)

> **Estado:** CONGELADO v1.1 — 2026-09-24 (v1.0: 2026-06-21). Cambios v1.1 (docs/fallos_firmware.md F1/F2/F3/F8/F4):
> `me` pasa a Chung–Pfost modificada en **% base húmeda**; nuevas claves opcionales `om` y `seg`;
> nuevos atributos `segmento`, `modelo`, `a`, `b`, `meModel`; `loteInicioTs` es el inicio del **segmento**.
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
- Modo de transporte: `config.h` → `TRANSPORT_MODE` (**LORAWAN** desde el 2026-09-24, firmware v1.2.0: gateway
  Heltec HT-M7603 + The Things Stack Cloud, ver `docs/LORAWAN.md`; **WIFI** sigue disponible para banco de pruebas).

---

## 1. Qué es crudo y qué es derivado

| Categoría | Campos | Origen |
|---|---|---|
| **Crudo (sensado)** | `t`, `rh`, `rain` | SHT31 (T°, HR) + YL-83 (lluvia digital) |
| **Derivado tipo A** (solo ambiente) | `me` | Chung–Pfost modificada (b.h.) — el edge lo calcula **siempre** (autónomo) |
| **Derivado tipo B** (contexto de lote) | `m`, `mr`, `dr`, `te`, `om`, `seg` | Page / Midilli por pasos — **requiere** parámetros aprovisionados por el backend (§3) |

> **Clave del modelo:** el ESP32 **no tiene sensor de humedad de grano**. `m = M(t)`
> se **modela** (Page o Midilli) a partir del tiempo de exposición del *segmento diario* y de `me`
> actual. **Convención (F2): todas las humedades de grano (`me`, `m`, `m0`, `mTarget`, `offsetM`) son
> % base húmeda (b.h.)**, la base del medidor PM-410 y del comercio.
>
> ```
> Me_bs = E − F · ln[ −(T + C) · ln(HR) ]              (Chung–Pfost modificada, % b.s.; T °C, HR fracción)
>         E = 29.394, F = 4.6015, C = 35.703          (ASAE D245.4, arroz cáscara; Basunia y Abe 2001)
> Me    = 100·Me_bs / (100 + Me_bs)                    (conversión a % b.h. — F2)
> MR(t) = exp( −k · tⁿ )                               (Page)      |  a·exp(−k·tⁿ) + b·t  (Midilli, modelo=MIDILLI)
> ```
>
> **Integración por pasos con tiempo equivalente (F10):** `Me` cambia cada ciclo, así que la forma
> cerrada NO se evalúa con el tiempo total. En cada publicación:
> ```
> MR_prev = (M_prev − Me) / (M₀ − Me)                  (M_prev = M del ciclo anterior, sin offset)
> si MR_prev ≤ 0 (grano en/bajo el equilibrio actual): M = M_prev   (sin secado; no se modela readsorción)
> si no: t_eq = MR⁻¹(MR_prev);  M = Me + (M₀ − Me)·MR(t_eq + Δt)     (Δt = horas desde la publicación anterior)
> mr  = (M − Me)/(M₀ − Me)                             (razón actual, acotada a [0,1])
> dr  = (M_prev − M)/Δt                    [%/h]        (entre dos publicaciones; se omite en la 1ª del segmento)
> te  = MR⁻¹(MR_obj) − (t_eq + Δt)         [h]          (MR_obj = (M_target − Me)/(M₀ − Me); 0 si inviable o cumplida)
> m   = M + offsetM                                     (offsetM debe ser 0 en campaña — F8; se publica en `om`)
> ```
> El **primer ciclo** del segmento (o tras un reinicio a mitad de segmento) usa la forma cerrada
> `M = Me + (M₀ − Me)·MR(t)` con `t = now − loteInicioTs`, única opción sin historial.
>
> Por eso el edge necesita del backend `M₀` (del segmento), `M_target`, `k`, `n` (y `modelo/a/b`) **y** el
> instante de inicio del segmento (§3). Sin lote activo, el edge solo emite los campos tipo A/crudos.
> Constantes de referencia: `firmware/oryzaflow_edge/config.h` (`CP_E/CP_F/CP_C`); el script
> `tools/replay_model.py` reproduce estas ecuaciones fuera de línea (F6).

**Conversiones de unidad dentro del edge (no salen en telemetría):**
- Chung–Pfost usa `HR` como **fracción** (`rh/100`, acotada a [0.01, 0.99]) y `T` en **°C**.
- Page/Midilli: `t` en **horas**; `k` y `n` deben haberse ajustado **en horas** (el backend
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
| `me`   | Humedad equilibrio (Me) | num | % b.h. | deriv A | —     | Chung–Pfost modificada, convertida a b.h.; recalculado **cada ciclo**. |
| `m`    | Humedad de grano (M(t)) | num | % b.h. | deriv B | 18–26→target | Modelado (Page/Midilli por pasos) + `offsetM`. Solo si lote activo. |
| `mr`   | Razón de humedad (MR)   | num | 0–1   | deriv B | 0–1        | `≤0.05` ⇒ proceso concluido. Solo si lote activo. |
| `dr`   | Tasa de secado (DR)     | num | %/h   | deriv B | 0.5–2.5    | `<0.5` lento; `>2.5` riesgo. Solo si lote activo. |
| `te`   | Tiempo restante estimado| num | h     | deriv B | —          | **Opcional pero recomendado** (app HU-curva). Solo si lote activo. |
| `om`   | offsetM vigente         | num  | % b.h. | deriv B | 0 (campaña) | **v1.1 (F8).** Corrección manual sumada a `m`; se publica para auditar el MAE. Solo si lote activo. |
| `seg`  | Segmento diario         | int  | —     | deriv B | 1, 2, …   | **v1.1 (F3).** Número del segmento aprovisionado (`segmento`, §3). Solo si lote activo y `segmento > 0`. |
| `lot`  | Lote activo en el edge  | bool | —     | meta   | —          | `false` ⇒ campos tipo B ausentes (Type A only). |

**Reglas de presencia:**
- Campos **siempre presentes:** `t`, `rh`, `rain`, `me`, `lot`.
- Campos tipo B (`m`, `mr`, `dr`, `te`, `om`, `seg`) **presentes solo si `lot=true`** (hay lote
  aprovisionado y `k, n > 0`). Nunca se envían como `null`: se **omiten**.
- `dr` se omite además en la **primera** publicación de cada **segmento** (no hay `M_prev`; F3).
- `seg` se omite si el backend no aprovisionó `segmento` (firmware/backend anteriores a v1.1).

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
  "mTarget": 12.0,
  "perfil": "caseritaNormal",
  "k": 0.1200,
  "n": 1.0500,
  "modelo": "PAGE",
  "a": 1.0,
  "b": 0.0,
  "segmento": 1,
  "meModel": "chung_pfost",
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
| `loteInicioTs` | int | ms epoch UTC | **v1.1:** inicio del **segmento diario** abierto (F3), no del lote. Ancla para `t` en **horas**; al cambiar, el edge reinicia `M_prev`. |
| `m0` | num | % b.h. | `M₀` del **segmento** (medida al descubrir; F3). Al cambiar, el edge reinicia `M_prev`. |
| `mTarget` | num | % b.h. | `M_target` para `MR_obj` y `te`. |
| `perfil` | string | — | Etiqueta comercial (§8 contexto); informativa. |
| `k` | num | (1/hⁿ) | Constante Page, **ajustada en horas**. |
| `n` | num | — | Exponente Page. |
| `modelo` | string | `PAGE` \| `MIDILLI` | **v1.1 (F4).** Modelo de capa delgada. Default `PAGE`. |
| `a`, `b` | num | — | **v1.1 (F4).** Constantes de Midilli `MR = a·exp(−k·tⁿ) + b·t`. Con `a=1, b=0` reproduce Page. |
| `segmento` | int | — | **v1.1 (F3).** Número del segmento diario; se refleja en `seg`. Al cambiar, el edge reinicia `M_prev`. |
| `meModel` | string | — | **v1.1 (F1).** Ecuación de equilibrio. Hoy solo `chung_pfost`; `henderson`/`steffe_singh` quedan `TODO(fuente)` hasta tener ASAE D245. |
| `intervaloEnvioSegundos` | int | s | Periodo de **publicación** (§4). `Esp32Config`. |
| `offsetT` | num | °C | Calibración SHT31 T (HU-44). `t = t_raw + offsetT`. |
| `offsetRh` | num | % | Calibración SHT31 HR. `rh = rh_raw + offsetRh`. |
| `offsetM` | num | % b.h. | Corrección del modelo `m` (def. 0). **Debe ser 0 en lotes de campaña** (F8); el backend rechaza iniciar con `offsetM ≠ 0` salvo `permitirOffsetM`. Se audita en `om`. |
| `tMax` | num | °C | Umbral de temperatura del lote/variedad (informativo). |

**Fin de lote / lote cubierto:** al completar/cancelar el lote **o al cerrar un segmento** (noche, lluvia; F3)
el backend borra los atributos del lote (`DELETE ... SHARED_SCOPE?keys=loteId,...`, que el edge recibe como
`{"deleted":["loteId",...]}`) o empuja `{"loteId": null}`. El edge vuelve a modo tipo A (sigue publicando
`t/rh/rain/me`). Al abrir el segmento siguiente el backend vuelve a empujar todo con el nuevo
`loteInicioTs`, `m0` y `segmento`.

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
| 15 | `seg` | uint8 | número de segmento | **v1.1** solo si `lot` y `hasSeg` |
| 16–17 | `om` | int16 | `round(om × 100)` | **v1.1** solo si `lot` y `hasOm` |

Longitudes resultantes: **7 bytes** (sin lote) · **16 bytes** (lote, 1ª pub del segmento: sin dr)
· **18 bytes** (lote completo con dr, te, seg y om). Los offsets de la tabla son los de la trama completa;
cuando un campo opcional falta, los siguientes se desplazan (el `decode()` usa `flags` para saberlo).

> **v1.1 (2026-09-24, firmware v1.2.0):** codec implementado en `firmware/oryzaflow_edge/codec.cpp`
> (`encodeTelemetry`), con `seg` y `om` incorporados. Uplink en **fPort 1**. Con lote y ADR en AU915 DR3 (SF9)
> caben 53 B, sobra margen.

### 5.2 Byte de flags (offset 0)

| Bit | Nombre | Significado |
|---|---|---|
| 0 | `lot`     | 1 = lote activo (vienen campos tipo B). |
| 1 | `rain`    | 1 = lluvia (YL-83). |
| 2 | `hasDr`   | 1 = `dr` presente en payload. |
| 3 | `hasTe`   | 1 = `te` presente en payload. |
| 4 | `fault`   | 1 = fallo lectura SHT31 (`t`/`rh` no fiables). |
| 5 | `hasSeg`  | **v1.1** 1 = `seg` presente (número de segmento diario, F3). |
| 6 | `hasOm`   | **v1.1** 1 = `om` presente (offsetM vigente, F8). |
| 7 | reservado | 0. |

### 5.3 Decoder de referencia — payload formatter de The Things Stack

El decoder canónico vive en **`lorawan/ttn_uplink_formatter.js`** (función `decodeUplink(input)` para la consola
de The Things Stack: *Applications → oryzaflow → Payload formatters → Uplink → Custom Javascript*). Reconstruye el
JSON de §2 con las mismas claves cortas:

```javascript
function decodeUplink(input) {
  var b = input.bytes;
  function u16(i) { return (b[i] << 8) | b[i + 1]; }
  function s16(i) { var v = u16(i); return v >= 0x8000 ? v - 0x10000 : v; }
  var flags = b[0], lot = (flags & 0x01) !== 0;
  var d = { t: s16(1) / 100, rh: u16(3) / 100, rain: (flags & 0x02) !== 0, me: u16(5) / 100, lot: lot };
  if (flags & 0x10) d.fault = true;
  var o = 7;
  if (lot) {
    d.m = u16(o) / 100; o += 2;
    d.mr = u16(o) / 10000; o += 2;
    if (flags & 0x04) { d.dr = s16(o) / 100; o += 2; }   // hasDr
    if (flags & 0x08) { d.te = u16(o) / 100; o += 2; }   // hasTe
    if (flags & 0x20) { d.seg = b[o]; o += 1; }          // hasSeg (v1.1)
    if (flags & 0x40) { d.om = s16(o) / 100; o += 2; }   // hasOm  (v1.1)
  }
  return { data: d };
}
```

> El `encode()` del firmware (`codec.cpp`, C++) es el inverso byte a byte de esto; el auto-test del
> firmware (caso (e)) comprueba la trama de 18 B del lote de referencia. La cuantización (×100 / ×10000) define la resolución: 0.01 en
> `t/rh/me/m/dr/te` y 0.0001 en `mr` — muy por encima de la precisión de los sensores
> y del modelo, así que el round-trip no pierde información útil.

### 5.4 Downlink de aprovisionamiento (modo LoRaWAN, fPort 10) — v1.1

En LoRaWAN los *shared attributes* de §3 no existen: el backend (o ThingsBoard vía su integración) encola un
downlink binario que el nodo interpreta en `provisioning.cpp` (`applyDownlink`). Encoder/decoder de referencia:
**`lorawan/ttn_downlink_formatter.js`** (permite encolar JSON `decoded_payload` desde la API o la consola de TTS).
Big-endian; primer byte = comando.

| cmd | Bytes | Campos (offset desde 1) | Equivale a (§3) |
|---|---|---|---|
| `SET_LOT` 0x01 | 23 | `inicioTs` u32 epoch s · `m0` u16×100 · `mTarget` u16×100 · `k` u16×10000 · `n` u16×1000 · `modelo` u8 (0 PAGE / 1 MIDILLI) · `a` u16×1000 · `b` i16×10000 · `offsetM` i16×100 · `segmento` u8 · `lotId` u16 | `loteId, loteInicioTs, m0, mTarget, k, n, modelo, a, b, offsetM, segmento` |
| `CLEAR_LOT` 0x02 | 1 | — | `{"loteId": null}` / borrado de claves |
| `SET_NODE` 0x03 | 7 | `offsetT` i16×100 · `offsetRh` i16×100 · `intervaloEnvioSegundos` u16 | `offsetT, offsetRh, intervaloEnvioSegundos` |
| `TIME_HINT` 0x04 | 5 | `epoch` u32 | — (respaldo de hora si la red no responde `DeviceTimeReq`) |

**Hora del nodo en LoRaWAN.** Sin WiFi no hay NTP: el nodo pide `DeviceTimeReq` (MAC) en el primer uplink tras
el join y cada 12 h; The Things Stack lo responde si el dispositivo está registrado como LoRaWAN **≥ 1.0.3**.
Con hora válida, `inicioTs` se ancla igual que `loteInicioTs` en WiFi; sin hora, `t=0` = instante de recepción
del `SET_LOT` (o se re-ancla cuando llega la hora). Los downlinks clase A se entregan tras el siguiente uplink
(≤ un periodo de publicación).

---

## 6. Atributos que el edge reporta de vuelta (client-side attributes)

Para alimentar el estado vivo del nodo en el móvil (`NodeModel`, `Esp32ConfigModel`).
Tópico `v1/devices/me/attributes` (WiFi) / metadatos LoRaWAN.

```json
{
  "fwVersion": "1.1.0",
  "transport": "wifi",
  "loteId": "LOTE-2026-0042",
  "rssi": -67,
  "ip": "192.168.1.42",
  "intervaloEnvioSegundos": 300,
  "bootCount": 12
}
```

Estos son **atributos**, no telemetría: cambian poco y se consultan por estado, no
por serie temporal. **Implementado en v1.1** (al conectar a MQTT): `fwVersion`, `transport`,
`intervaloEnvioSegundos`, `ip`, `rssi`. Pendientes: `loteId`, `bootCount`.

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
| `om` | — | — | `offset_m` (v1.1, auditoría F8) |
| `seg` | — | — | `segmento` (v1.1, F3) |
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

- [x] v1.1 (2026-09-24): `me/m` en % b.h. con Chung–Pfost; claves `om`, `seg`; atributos `segmento`,
  `modelo`, `a`, `b`, `meModel`; integración por pasos (§1). Firmware v1.1.0 en C++/Arduino
  (`firmware/oryzaflow_edge`, no MicroPython).

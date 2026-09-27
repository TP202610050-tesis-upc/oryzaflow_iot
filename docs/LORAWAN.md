# OryzaFlow — Transporte LoRaWAN (Etapa 4b/4c)

> Estado 2026-09-24 19:45: firmware v1.2.0 con `TRANSPORT_MODE = TRANSPORT_LORAWAN` **verificado en radio**:
> join OTAA OK, uplink de 7 B, `DeviceTimeReq` respondido (epoch 1790298343), downlink RSSI −79 / SNR 9 con el
> gateway HT-M7603 conectado a The Things Stack Cloud (tenant `oryzaflow`, `nam1`). Camino de datos elegido:
> **B — TTS → integración The Things Stack de ThingsBoard → Rule Chain actual → HiveMQ → backend** (§5).
> Licencia Heltec del chip A453759E9EF0 en `secrets.h` (`HELTEC_LICENSE`).

## 1. Red

| Elemento | Valor |
|---|---|
| Servidor de red (LNS) | The Things Stack Cloud, plan Discovery; consola `https://oryzaflow.nam1.cloud.thethings.industries/console/` |
| Plan de frecuencias | **Australia 915-928 MHz, FSB 2 (used by TTN)** = AU915 canales 8-15 (916,8–917,9 MHz) + 65 |
| Gateway | Heltec HT-M7603, EUI `E43819FFFE1F7A72`, ID `oryzaflow-gw-01`, UDP packet forwarder → `oryzaflow.nam1.cloud.thethings.industries:1700`, región AU915, canales 8-15,65 |
| Aplicación | `oryzaflow` |
| Nodo | `oryzaflow-edge-01`, OTAA, LoRaWAN 1.0.2 / RP001 1.0.2 rev B, clase A, JoinEUI `0000000000000000`; DevEUI y AppKey en `firmware/oryzaflow_edge/secrets.h` (fuera de git) |

**Hora de red.** El nodo pide `DeviceTimeReq` en el primer uplink tras el join y cada 12 h. The Things Stack solo
responde si la versión LoRaWAN del dispositivo es **≥ 1.0.3**: en la consola, *End device → General settings →
Network layer → LoRaWAN version* → **1.0.3**, *Regional Parameters* → **RP001 1.0.3 revision A**. (La pila de Heltec
es 1.0.2 pero el join/clase A es compatible.) Respaldo si no llega la hora: downlink `TIME_HINT` (§3).

## 2. Firmware

- `config.h`: `TRANSPORT_MODE` (WIFI/LORAWAN), `LORAWAN_UPLINK_PORT=1`, `LORAWAN_DOWNLINK_PORT=10`,
  `LORAWAN_CHANNEL_MASK` (FSB2), `LORAWAN_DEFAULT_DR=3` (SF9, hasta 53 B; el uplink v1.1 usa ≤ 18 B), ADR activo.
- `net_lorawan.cpp`: pila de Heltec (`LoRaWan_APP.h`) **sin `LoRaWAN.sleep()`** (esa función entra en deep sleep y
  borraría el estado del modelo en RAM); los temporizadores y las IRQ de la radio se atienden en cada vuelta del
  loop. Uplinks no confirmados. Durante `LORAWAN_TX_BUSY_MS` (12 s: TX + RX1 5 s + RX2) el loop no hace trabajo pesado.
- `codec.cpp`: codec binario v1.1 (ESQUEMA §5). `provisioning.cpp`: `applyDownlink()` (§5.4) y reloj del nodo.
- El JSON canónico se sigue imprimiendo por serial en cada publicación (sirve para `tools/replay_model.py`).

Compilar y subir (la región va en el menú de placa, `LORAWAN_REGION=4` = AU915):

```powershell
$cli  = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$fqbn = "Heltec-esp32:esp32:heltec_wifi_lora_32_V3:LORAWAN_REGION=4"
& $cli compile --fqbn $fqbn --upload -p COM8 .\firmware\oryzaflow_edge
```

Serial esperado: `[transporte] LoRaWAN`, auto-test 5/5 PASS (incluye el caso (e) del codec), `joining...`,
`[lora] unido (OTAA)`, `[lora] DeviceTimeReq anexado al uplink`, `{"t":…}` + `[lora] uplink 7 B publish=ok`.
Sin lote el uplink es de 7 B; con lote, 16–18 B.

**Latido con sensor en falla (firmware 1.2.1, 27/09/2026).** Si el SHT31 no responde, el nodo no calcula ni publica
telemetría, pero en LoRaWAN envía igualmente un uplink de **1 B = `0x10`** (bandera `fault`) en cada ciclo: mantiene
la ventana RX (los downlinks de aprovisionamiento siguen llegando) y deja rastro del fallo. Tratamiento aguas abajo:
- `ttn_uplink_formatter.js` lo decodifica como `{ "fault": true }` con un warning.
- `tb_uplink_converter.js` **no genera telemetría** para esa trama: la registra como atributos del device
  `sensorFault = true` y `sensorFaultTs`; así la Rule Chain no la reenvía a HiveMQ.
- El backend, por si acaso, descarta cualquier trama sin `t`/`rh` (`TelemetryIngestionService`, commit `047c508`):
  no crea lectura ni marca el nodo ACTIVO, de modo que el watchdog lo lleva a DESCONECTADO («Revisar hardware o
  energía») si el fallo persiste.
El converter de TB se actualiza a mano en *Integrations center → Data converters → TTS uplink oryzaflow* pegando el
contenido actual de `lorawan/tb_uplink_converter.js` (probar con `frm_payload` = `EA==`, es decir `[0x10]`: debe
salir `telemetry: []` y `attributes.sensorFault: true`).

## 3. Payload formatters (consola TTS → Applications → oryzaflow → Payload formatters)

- **Uplink**: `lorawan/ttn_uplink_formatter.js` → reconstruye el JSON canónico (`t, rh, rain, me, lot, m, mr, dr, te, seg, om`).
- **Downlink**: `lorawan/ttn_downlink_formatter.js` → el backend encola JSON (`decoded_payload`) y TTS genera los bytes:

| cmd | Campos | Efecto en el nodo |
|---|---|---|
| `SET_LOT` | `inicioTs` (epoch s), `m0`, `mTarget`, `k`, `n`, `modelo`, `a`, `b`, `offsetM`, `segmento`, `lotId` | equivale a los shared attributes de §3: activa el lote, ancla `t=0` al `inicioTs` (si hay hora de red), reinicia `M_prev` si cambia lote/segmento/m0 |
| `CLEAR_LOT` | — | fin de lote / lote cubierto → tipo A |
| `SET_NODE` | `offsetT`, `offsetRh`, `intervaloEnvioSegundos` | calibración y cadencia |
| `TIME_HINT` | `epoch` (opcional; TTS pone "ahora") | fija el reloj si la red no respondió `DeviceTimeReq` (exactitud ≈ un periodo de publicación) |

Encolar a mano desde la consola: *End device → Messaging → Downlink → fPort 10 → JSON* (con el formatter cargado)
o *Bytes* (`02` = CLEAR_LOT). Los downlinks clase A se entregan tras el siguiente uplink (≤ 5 min).

## 4. Prueba de humo

1. Consola → gateway `oryzaflow-gw-01` → Live data: uplinks del nodo (`Forward uplink data message`).
2. Consola → end device → Live data: `decoded_payload` con `t/rh/me/lot=false`.
3. Encolar `{"cmd":"TIME_HINT"}` y luego `{"cmd":"SET_LOT","inicioTs":<epoch>,"m0":22,"mTarget":12,"segmento":1}`:
   en el serial `[prov] SET_LOT …` y el siguiente uplink lleva `lot=true`, `m≈22`, `seg=1`, `om=0`.
4. `{"cmd":"CLEAR_LOT"}` → siguiente uplink `lot=false`.

## 5. Camino de datos B: TTS → ThingsBoard → HiveMQ → backend (y shared attributes → downlink)

El plan de thingsboard.cloud del proyecto permite **1 integración y 3 converters** (suficiente). El backend no
cambia: sigue ingiriendo de HiveMQ `oryzaflow/telemetry/<deviceName>` y sigue aprovisionando por shared attributes
de TB; la integración hace la traducción en ambos sentidos. `deviceName` = `oryzaflow-edge-01` (= `Node.serialEsp32`).

### 5.1 En The Things Stack (consola → Applications → oryzaflow)
1. **Payload formatters → Uplink**: *Custom Javascript formatter*, pegar `lorawan/ttn_uplink_formatter.js`. (Opcional
   pero útil: la consola muestra el JSON decodificado.) **Downlink**: pegar `lorawan/ttn_downlink_formatter.js`.
2. **API keys → Add API key**: nombre `thingsboard`, derechos *Read application traffic (uplink)* y *Write downlink
   application traffic*. Copiar la clave (solo se muestra una vez).

### 5.2 En ThingsBoard (thingsboard.cloud)
1. **Integrations center → Data converters → +**: tipo *Uplink*, nombre `TTS uplink oryzaflow`, pegar
   `lorawan/tb_uplink_converter.js`. Otro *Downlink*, nombre `TTS downlink oryzaflow`, pegar
   `lorawan/tb_downlink_converter.js`.
2. **Integrations center → Integrations → +**: tipo **The Things Stack** (Industries/Cloud). Uplink converter y
   downlink converter = los anteriores. Región/host: `nam1.cloud.thethings.industries`; **Application ID**
   `oryzaflow`; **Tenant ID** `oryzaflow`; API key = la de §5.1. Guardar y comprobar que la integración queda *Active*.
3. Con el nodo publicando (cada 5 min), en **Devices → oryzaflow-edge-01 → Latest telemetry** deben aparecer
   `t, rh, me, lot` (y `rssi/snr`). Si la integración creó un device nuevo en vez de usar el existente, el
   `deviceName` del converter no coincide con el nombre del device; corregirlo.
4. **Rule chains → Root** (la que reenvía *Post telemetry* a HiveMQ): añadir al *Message Type Switch* las salidas
   **Attributes Updated** y **Attributes Deleted** → nodo **integration downlink** (integración The Things Stack).
   Así `ProvisioningService.provision()` (escribe shared attributes) genera un `SET_LOT` y `clear()` (borra claves)
   un `CLEAR_LOT`, sin tocar el backend. *(Hecho el 25/09/2026 por la API REST de TB: nodo
   `to TTS (downlink aprovisionamiento)`, tipo `TbIntegrationDownlinkNode`.)*

   **Lecciones de la puesta en marcha (25/09/2026):**
   - El tópico de downlink de la integración es `v3/oryzaflow@oryzaflow/devices/${devId}/down/push` (no editable):
     el converter de downlink **debe devolver `devId` en `metadata`**; sin él la integración descarta el mensaje en
     silencio. `tb_downlink_converter.js` ya lo incluye.
   - La depuración de la integración se activa por 15 min (botón "15 min"); vencida, solo se registran fallos. Para
     ver los eventos *Downlink* hay que reactivarla antes de probar.
   - Con `down/push`, cada `Attributes Updated` encola un downlink; TTS los entrega de uno en uno (uno por uplink).
     El nodo ignora repeticiones sin cambios de lote/segmento/m0.

### 5.3 Prueba (ejecutada el 25/09/2026 con LOTE-3 de prueba en RDS)
- 18:12:34 `POST /batches/3/start` → shared attributes en TB → rule chain → integración → TTS (`down/push`, 23 B).
- 18:18:24 uplink del nodo (7 B, `lot=false`) → 18:18:29 RX1: `[prov] SET_LOT lote=3 seg=1 m0=22.00 mTarget=12.00`
  (RSSI −42, SNR 11) → `[prov] lote/segmento cambiado (gen=1)`.
- Subida verificada antes: nodo → TTS → TB (`t/rh/me/lot/rssi/snr`) → HiveMQ (`oryzaflow/telemetry/oryzaflow-edge-01`)
  → backend local contra RDS (`sensor_readings`, nodo `Edge Real` ACTIVO).
- `POST /api/v1/batches/{id}/start` en el backend → TB shared attributes → downlink `SET_LOT` → en el serial del nodo
  `[prov] SET_LOT …` (tras su siguiente uplink, ≤ 5 min) → siguiente uplink con `lot=true`, `m≈m0`, `seg=1`.
- `segments/close {NOCHE}` → `CLEAR_LOT` → `lot=false`. `POST /segments {m0}` → `SET_LOT` con `segmento=2`.
- En RDS: `sensor_readings` con `segmento` y `offset_m`.

### Alternativa A (no elegida)
Puente propio (`bridge/`): suscriptor MQTT de TTS (`v3/oryzaflow@oryzaflow/devices/+/up`) → HiveMQ, y el backend
llamaría a la API de downlinks de TTS con `decoded_payload`. Queda documentado por si el plan de TB cambia.

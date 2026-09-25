# OryzaFlow — Transporte LoRaWAN (Etapa 4b/4c)

> Estado 2026-09-24: firmware v1.2.0 con `TRANSPORT_MODE = TRANSPORT_LORAWAN`. Gateway HT-M7603 registrado y
> conectado a The Things Stack Cloud (tenant `oryzaflow`, clúster `nam1`). Nodo `oryzaflow-edge-01` registrado (OTAA).
> Camino de datos TTS → backend: **pendiente de decidir** (§5).

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

## 5. Pendiente: camino de datos TTS → backend y backend → nodo

- **A. Puente propio** (`bridge/`): suscriptor MQTT de TTS (`mqtt://nam1.cloud.thethings.industries:1883`,
  usuario `oryzaflow@ttn` / API key, tópico `v3/oryzaflow@ttn/devices/+/up`) que publica el `decoded_payload` en HiveMQ
  `oryzaflow/telemetry/<deviceName>` → el backend ingiere sin cambios. Aprovisionamiento: el backend llama a la API
  de downlinks de TTS (`POST /api/v3/as/applications/oryzaflow/devices/oryzaflow-edge-01/down/replace` con
  `decoded_payload` SET_LOT/CLEAR_LOT) en lugar de escribir shared attributes en ThingsBoard.
- **B. Integración TTN de ThingsBoard** (requiere que el plan de thingsboard.cloud tenga *Integrations*): conserva
  la cadena actual (TB → HiveMQ → backend) y los shared attributes, que TB traduce a downlinks con un conversor.

El `deviceName` que el backend usa para resolver el nodo es `oryzaflow-edge-01` (= `Node.serialEsp32`).

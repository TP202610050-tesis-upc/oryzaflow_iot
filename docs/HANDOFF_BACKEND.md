# OryzaFlow — Handoff IoT → Backend

> Para la sesión de **backend** (Spring Boot). Resume qué quedó listo en el lado IoT
> y qué necesita el backend para ingerir/aprovisionar. Lee junto con
> `ESQUEMA_TELEMETRIA.md` (contrato de telemetría congelado v1.0).

## 1. Estado del IoT (2026-06-21)

- Edge **Heltec WiFi LoRa 32 V3** publicando telemetría **real y en vivo**.
- ThingsBoard: cuenta en **thingsboard.cloud**, device **`oryzaflow-edge-01`**,
  conexión MQTT `mqtt.thingsboard.cloud:1883` (TLS 8883 pendiente).
- El JSON que llega al device es **exactamente** el del esquema (claves cortas,
  reglas de presencia tipo A/B).

## 2. Payloads reales (capturados del edge)

Lote activo (caso normal); `te:0` porque con HR ambiente alta `Me ≥ M_target` (meta inviable):
```json
{"t":24.98,"rh":66.20,"rain":false,"me":16.86,"lot":true,"m":21.99,"mr":0.9980,"dr":0.53,"te":0.00}
```
Primera publicación del lote (sin `dr`):
```json
{"t":25.07,"rh":66.08,"rain":false,"me":16.83,"lot":true,"m":22.00,"mr":0.9995,"te":0.00}
```
Sin lote activo (solo tipo A — el edge sigue latiendo):
```json
{"t":25.97,"rh":69.40,"rain":false,"me":17.30,"lot":false}
```
Caso seco/feasible (del self-test, T=42/HR=45):
```json
{"t":42.00,"rh":45.00,"rain":false,"me":11.75,"lot":true,"m":17.10,"mr":0.5219,"dr":0.78,"te":10.29}
```

## 3. ⚠️ Pieza que el backend debe resolver: el puente ThingsBoard → backend

El diseño dice "backend = suscriptor MQTT de ThingsBoard", pero **ThingsBoard NO
permite que un cliente MQTT externo se suscriba a la telemetría de un device**
(la API MQTT de device es solo para que el device publique/atienda RPC). Para sacar
los datos hacia Spring Boot hay que crear un **Rule Chain** en ThingsBoard. Decidir:

| Opción | Mecanismo | Nota |
|---|---|---|
| **A. REST webhook** | Rule Chain → *REST API Call* → `POST /api/v1/telemetry/ingest` | Más simple, sin broker; descartada para respetar el diseño. |
| **B. MQTT externo ✅ ELEGIDA** | Rule Chain → *MQTT node* → broker propio (Mosquitto/EMQX) → backend suscribe (Paho) | Respeta el diseño "suscriptor MQTT". Añade un broker a operar. |
| **C. Polling REST** | Backend consulta REST API de TB | No-push; evitar. |

> **DECISIÓN (2026-06-21): opción B** — para respetar el diseño "backend = suscriptor
> MQTT". La nota de [[oryzaflow-backend-stack]] (Paho/spring-integration-mqtt) **sí aplica**.

### Contrato de ingesta (opción B)
- Broker MQTT propio (p. ej. Mosquitto/EMQX) entre ThingsBoard y el backend.
- ThingsBoard: **Rule Chain** con un nodo *MQTT* que publica la telemetría del device a
  un tópico del broker, p. ej. `oryzaflow/telemetry/{deviceName}`.
- Backend: cliente **Paho / spring-integration-mqtt** suscrito a ese tópico.
- Payload: el JSON del §2 (configurar el nodo para incluir `deviceName`).
- El backend mapea claves cortas → `LECTURA` y último valor del nodo (ver `ESQUEMA §7`).
- Reglas: tolerar ausencia de campos tipo B (lote inactivo) y de `dr` (1ª lectura).
- **Pendiente IoT (próxima sesión):** levantar el broker y crear el Rule Chain en TB.

## 4. Aprovisionamiento backend → edge (sentido inverso)

Al **iniciar un lote** (BC4), el backend debe escribir *shared attributes* del device
vía **REST API de ThingsBoard**:
```
POST https://thingsboard.cloud/api/plugins/telemetry/DEVICE/{deviceId}/SHARED_SCOPE
Authorization: Bearer <JWT de usuario TB>
Body: { "loteId":"...", "loteInicioTs":<ms>, "m0":22.5, "mTarget":13.0,
        "perfil":"caseritaNormal", "k":0.12, "n":1.05, "intervaloEnvioSegundos":300,
        "offsetT":0, "offsetRh":0, "offsetM":0, "tMax":45 }
```
(El edge consumirá esto en la Etapa 4 del firmware; hoy el lote está hardcodeado.)
Campos y semántica exactos en `ESQUEMA_TELEMETRIA.md §3`.

## 5. Qué puede empezar el backend YA (sin esperar el puente)

- DTOs/serialización y persistencia (`LECTURA`) contra los payloads del §2.
- Mapeo claves→modelo (`ESQUEMA §7`), motor de alertas (BC5) sobre los rangos (§9 contexto).
- El servicio de aprovisionamiento (§4) — solo necesita el `deviceId` y un JWT de TB.
- Tests con los JSON reales del §2.

Lo único que requiere el puente (§3) es el **flujo end-to-end en vivo**; la lógica se
desarrolla y testea sin él.

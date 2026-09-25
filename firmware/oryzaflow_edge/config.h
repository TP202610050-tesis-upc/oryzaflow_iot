#pragma once
// ============================================================================
// OryzaFlow edge — configuración de hardware y modelo de secado
// Placa: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262, OLED SSD1306 128x64)
// Ver contrato de telemetría: oryzaflow_iot/docs/ESQUEMA_TELEMETRIA.md
// ============================================================================

// ---- Pines de sensores (cableado de Harold) ----
#define PIN_SHT31_SDA   41   // SHT31 SDA  (I2C bus propio, separado del OLED 17/18)
#define PIN_SHT31_SCL   42   // SHT31 SCL
#define PIN_RAIN_DO      6   // YL-83 salida DIGITAL  -> rain (bool)
#define PIN_RAIN_AO      7   // YL-83 salida ANALÓGICA -> intensidad (extra, no va al esquema)
#define SHT31_I2C_ADDR  0x44 // ADDR a GND (0x45 si ADDR a VDD)

// YL-83: el comparador pone DO en LOW cuando detecta lluvia (activo-bajo)
#define RAIN_ACTIVE_LOW  1

// ---- Calibración (offsets, HU-44). El aprovisionamiento del backend los sobreescribe. ----
#define DEFAULT_OFFSET_T   0.0f   // °C
#define DEFAULT_OFFSET_RH  0.0f   // %
#define DEFAULT_OFFSET_M   0.0f   // % grano (corrección del modelo)

// ---- Transporte (ESQUEMA §0): el JSON canónico es el contrato; el medio físico se conmuta aquí ----
//   TRANSPORT_WIFI    : JSON por MQTT a ThingsBoard (Etapa 3); aprovisionamiento por shared attributes; hora por NTP.
//   TRANSPORT_LORAWAN : payload binario (codec.h) por LoRaWAN OTAA clase A a The Things Stack (Etapa 4b/4c);
//                       aprovisionamiento por downlink binario (§5.4); hora por DeviceTimeReq / TIME_HINT.
#define TRANSPORT_WIFI     0
#define TRANSPORT_LORAWAN  1
#define TRANSPORT_MODE     TRANSPORT_LORAWAN

// ---- LoRaWAN (compilar con el menú de placa LORAWAN_REGION = REGION_AU915; FQBN ...:LORAWAN_REGION=4) ----
#define LORAWAN_UPLINK_PORT        1        // fPort de la telemetría (uplink)
#define LORAWAN_DOWNLINK_PORT      10       // fPort del aprovisionamiento (downlink)
#define LORAWAN_ADR                true     // la red ajusta DR/potencia
#define LORAWAN_DEFAULT_DR         3        // AU915 DR3 = SF9/125 kHz: 53 B de carga (el uplink v1.1 usa <= 18 B)
// Sub-banda 2 de AU915 (la que usa The Things Stack): canales 8-15 (916.8-917.9 MHz) + canal 65 (500 kHz).
#define LORAWAN_CHANNEL_MASK       { 0xFF00, 0x0000, 0x0000, 0x0000, 0x0002, 0x0000 }
#define LORAWAN_TX_BUSY_MS         12000UL  // TX + RX1 (5 s en TTS) + RX2 (6 s) + margen: sin trabajo pesado
#define LORAWAN_REJOIN_DELAY_MS    30000UL
#define LORAWAN_TIME_REQ_INTERVAL_MS (12UL * 3600UL * 1000UL)   // DeviceTimeReq cada 12 h (deriva de millis)

// ---- Frecuencia (ver §4 del esquema) ----
#define SAMPLE_INTERVAL_MS   60000UL  // muestreo de sensores (suaviza ruido)
#define PUBLISH_INTERVAL_S   300      // publicación de telemetría 5 min (§4; configurable por atributo/downlink)

// ---- Constantes del modelo (CONTEXTO_PROYECTO §7; docs/fallos_firmware.md F1/F2) ----
// Humedad de equilibrio: Chung-Pfost modificada para arroz cascara (forma ASAE D245.4):
//   Me_bs [% base seca] = CP_E - CP_F * ln[ -(T + CP_C) * ln(HR) ]     T en °C, HR en fraccion (0-1)
//   equivalente a HR = exp[-A/(T+C) * exp(-B*M)] con A=594.85, B=21.732, C=35.703 y M en decimal b.s.
//   (E = 100*ln(A)/B = 29.394, F = 100/B = 4.6015).
// Fuente de las constantes: Basunia, M. A. y Abe, T. (2001). Thin-layer solar drying characteristics of
// rough rice under natural convection. J. Food Eng. 47(4), 295-301 (usan ASAE D245.4).
// TODO(fuente): confirmar contra el texto de ASAE D245.4/D245.5 cuando el equipo lo tenga (PENDIENTES 1.6).
// La ecuacion devuelve % base SECA; el edge la convierte a % base HUMEDA (moisture_bs_to_bh) antes de usarla.
#define CP_E  29.394f
#define CP_F  4.6015f
#define CP_C  35.703f

// Alternativas (Henderson modificada, Steffe-Singh) seleccionables por el atributo compartido meModel:
// TODO(fuente): sin constantes citadas de ASAE D245 no se implementan; hoy solo "chung_pfost".

// Guardas numericas de la integracion por pasos (F10)
#define MODEL_EPS      1e-4f   // (M0 - Me) por debajo de esto => no hay potencial de secado
#define MODEL_MR_MIN   1e-6f   // cota inferior de MR_prev antes de invertir el modelo
#define MODEL_T_MAX_H  500.0f  // horizonte de la biseccion (Midilli general) [h]

// ---- Meta del lote y banda de conformidad (Protocolo de campo v1.0, 2026-09-21) ----
// Convención: TODAS las humedades de grano (m0, mTarget, m, me, offsetM) se expresan en % base húmeda (b.h.).
// La meta la aprovisiona el backend (mTarget). Campaña 2026: una variedad de arroz común y una meta única en el
// rango 11-13 % b.h. (valor provisional 12.0; el encargado lo confirma con decimal en la primera visita).
// TODO(campo): el 9 % de saco de marca queda fuera del alcance de la campaña (decisión del 22/09/2026).
// Banda de conformidad: lote conforme si |M_final - mTarget| <= TARGET_TOLERANCE_PP. La evalúa el backend
// (oryzaflow.drying.target-tolerance-pp); aquí se declara para que el edge y el backend compartan el valor.
#define TARGET_TOLERANCE_PP  0.5f   // pp, por confirmar con el encargado de la planta

// Escala de tiempo del modelo Page. 1.0 = tiempo real (producción).
// Subir (p. ej. 720) SOLO para demo: acelera la curva de secado y hace visible
// el avance de M/DR/te en una prueba corta de escritorio.
#define MODEL_TIME_SCALE  1.0f   // tiempo REAL (producción). Subir solo para demos de escritorio.

// ---- Aprovisionamiento (Etapa 4a) ----
// Reloj real por NTP (para mapear loteInicioTs absoluto -> t del modelo).
#define NTP_SERVER  "pool.ntp.org"
// Claves que el edge pide a ThingsBoard (deben coincidir con ProvisioningService del backend).
#define TB_SHARED_KEYS  "loteId,loteInicioTs,m0,mTarget,perfil,k,n,modelo,a,b,segmento,meModel,intervaloEnvioSegundos,offsetT,offsetRh,offsetM,tMax"

// ---- Version de firmware (client attribute fwVersion, ESQUEMA §6; docs/equivalencia_nodo_offline.md) ----
#define FW_VERSION  "1.2.0"

// ---- Serial ----
#define SERIAL_BAUD  115200

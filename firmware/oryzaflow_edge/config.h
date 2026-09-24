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

// ---- Frecuencia (ver §4 del esquema) ----
#define SAMPLE_INTERVAL_MS   60000UL  // muestreo de sensores (suaviza ruido)
#define PUBLISH_INTERVAL_S   150      // publicación de telemetría 2.5 min (configurable por atributo)

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
#define FW_VERSION  "1.1.0"

// ---- Serial ----
#define SERIAL_BAUD  115200

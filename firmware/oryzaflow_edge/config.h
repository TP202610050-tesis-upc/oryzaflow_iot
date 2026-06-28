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

// ---- Constantes del modelo (CONTEXTO_PROYECTO §7) ----
// Henderson modificado:  Me = ( -ln(1-HR) / (A*(T+C)) )^(1/B)   con HR en fracción, T en °C
#define HENDERSON_A  2.448e-5f
#define HENDERSON_B  2.334f
#define HENDERSON_C  35.703f

// Escala de tiempo del modelo Page. 1.0 = tiempo real (producción).
// Subir (p. ej. 720) SOLO para demo: acelera la curva de secado y hace visible
// el avance de M/DR/te en una prueba corta de escritorio.
#define MODEL_TIME_SCALE  1.0f   // tiempo REAL (producción). Subir solo para demos de escritorio.

// ---- Aprovisionamiento (Etapa 4a) ----
// Reloj real por NTP (para mapear loteInicioTs absoluto -> t del modelo).
#define NTP_SERVER  "pool.ntp.org"
// Claves que el edge pide a ThingsBoard (deben coincidir con ProvisioningService del backend).
#define TB_SHARED_KEYS  "loteId,loteInicioTs,m0,mTarget,perfil,k,n,intervaloEnvioSegundos,offsetT,offsetRh,offsetM,tMax"

// ---- Serial ----
#define SERIAL_BAUD  115200

// ============================================================================
// OryzaFlow edge — firmware (Heltec WiFi LoRa 32 V3 / ESP32-S3)
// ETAPA 2: sensores -> modelo de secado -> JSON canónico de telemetría.
// (Etapa 1 sensores: OK. Falta: OLED, transporte WiFi/LoRaWAN, aprovisionamiento.)
// Esquema: oryzaflow_iot/docs/ESQUEMA_TELEMETRIA.md
// ============================================================================
#include "config.h"
#include "sht31.h"
#include "drying_model.h"
#include "telemetry.h"
#include "display_oled.h"
#include "net_thingsboard.h"

static Sht31 sht(Wire1, SHT31_I2C_ADDR);

// Parámetros del lote. EN ETAPA 2 se fijan aquí a mano (placeholder);
// en la Etapa 4 los empuja el backend por shared attributes.
static LotParams g_lot;

// Offsets de calibración vigentes (los sobreescribirá el aprovisionamiento).
static float g_offsetT  = DEFAULT_OFFSET_T;
static float g_offsetRH = DEFAULT_OFFSET_RH;
static float g_offsetM  = DEFAULT_OFFSET_M;

// Estado para DR (M de la publicación anterior).
static float g_prevM = -1.0f;

// Cadencia de publicación de producción (PUBLISH_INTERVAL_S = 300 s, ver §4 esquema).
static const uint32_t PUBLISH_MS = (uint32_t)PUBLISH_INTERVAL_S * 1000UL;
static uint32_t g_lastPublish = 0;
static bool     g_firstPublish = true;  // publica pronto al arrancar, luego cada PUBLISH_MS

static void i2cScan(TwoWire& bus) {
  Serial.print("[i2c] escaneo bus SHT31 (41/42): ");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) { Serial.printf("0x%02X ", a); n++; }
  }
  Serial.println(n ? "" : "(nada encontrado)");
}

static bool rainDetected() {
  return RAIN_ACTIVE_LOW ? (digitalRead(PIN_RAIN_DO) == LOW)
                         : (digitalRead(PIN_RAIN_DO) == HIGH);
}

// Auto-test determinista del modelo (verificable a mano). Lote feasible:
// M0=22, M_target=13, k=0.12, n=1.05; ambiente T=42 °C, HR=45 %.
// Esperado @t=5h: me≈11.75, mr≈0.522, m≈17.10, te≈10.3; dr(@4h->5h)≈0.78 %/h.
static void selfTestModel() {
  Serial.println("--- self-test modelo (esperado: me~11.75 m~17.10 mr~0.522 te~10.3 dr~0.78) ---");
  LotParams lp;
  lp.active = true; lp.startMs = 0;
  lp.m0 = 22.0f; lp.mTarget = 13.0f; lp.k = 0.12f; lp.n = 1.05f;

  char buf[256];
  // t=4h (sin prevM -> sin dr): fija M previa
  DryingState a = computeDryingState(lp, 42.0f, 45.0f, false, 4.0 * 3600000.0, -1.0f, 0);
  buildTelemetryJson(buf, sizeof(buf), a, 0);
  Serial.printf("  t=4h: %s\n", buf);
  // t=5h con prevM=M(4h), dt=1h -> dr
  DryingState b = computeDryingState(lp, 42.0f, 45.0f, false, 5.0 * 3600000.0, a.m, 1.0f);
  buildTelemetryJson(buf, sizeof(buf), b, 0);
  Serial.printf("  t=5h: %s\n", buf);
  Serial.println("--- fin self-test ---");
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(400);
  Serial.println("\n=== OryzaFlow edge — Etapa 2: modelo -> JSON ===");

  pinMode(PIN_RAIN_DO, INPUT);
  analogReadResolution(12);

  displayBegin();

  Wire1.begin(PIN_SHT31_SDA, PIN_SHT31_SCL, 100000);
  i2cScan(Wire1);
  Serial.println(sht.begin() ? "[sht31] OK" : "[sht31] WARN: no responde");

  selfTestModel();

  // Lote de prueba ACTIVO desde el arranque (placeholder de aprovisionamiento).
  g_lot.active  = true;
  g_lot.startMs = 0;            // inicio = boot
  g_lot.m0      = 22.0f;
  g_lot.mTarget = 13.0f;
  g_lot.k       = 0.12f;
  g_lot.n       = 1.05f;
  g_lot.offsetM = g_offsetM;
  Serial.println("[lote] placeholder activo: M0=22 Mt=13 k=0.12 n=1.05");

  netBegin();  // WiFi + MQTT a ThingsBoard
}

void loop() {
  netLoop();  // mantiene WiFi/MQTT vivos (no bloqueante)

  uint32_t now = millis();
  if (!g_firstPublish && (now - g_lastPublish < PUBLISH_MS)) { delay(20); return; }
  g_firstPublish = false;

  float dtHours = (now - g_lastPublish) / 3600000.0f * MODEL_TIME_SCALE;
  g_lastPublish = now;

  Sht31Reading s = sht.read();
  if (!s.ok) { Serial.println("{\"err\":\"sht31\"}"); return; }
  float t  = s.tempC  + g_offsetT;
  float rh = s.humPct + g_offsetRH;
  bool  rain = rainDetected();

  DryingState st = computeDryingState(g_lot, t, rh, rain, (double)now, g_prevM, dtHours);
  if (st.hasB) g_prevM = st.m;

  char json[256];
  buildTelemetryJson(json, sizeof(json), st, 0);  // sin ts: ThingsBoard sella
  Serial.println(json);

  bool pub = netPublish(json);
  Serial.printf("[mqtt] publish=%s estado=%s\n", pub ? "ok" : "NO", netStatus());

  displayRender(st, netStatus());
}

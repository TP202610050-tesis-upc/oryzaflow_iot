// ============================================================================
// OryzaFlow edge — firmware (Heltec WiFi LoRa 32 V3 / ESP32-S3)
// Sensores -> modelo de secado -> JSON canónico -> ThingsBoard (WiFi/MQTT).
// ETAPA 4a: el lote lo aprovisiona el backend por shared attributes (sin hardcode).
// Esquema: oryzaflow_iot/docs/ESQUEMA_TELEMETRIA.md
// ============================================================================
#include "config.h"
#include "sht31.h"
#include "drying_model.h"
#include "telemetry.h"
#include "display_oled.h"
#include "net_thingsboard.h"
#include "provisioning.h"

static Sht31 sht(Wire1, SHT31_I2C_ADDR);

// Parámetros del lote: los aprovisiona el backend por shared attributes (Etapa 4a).
// Arranca INACTIVO -> el edge publica solo tipo A hasta que llegue un lote.
static LotParams g_lot;

// Offsets de calibración vigentes (los sobreescribirá el aprovisionamiento).
static float g_offsetT  = DEFAULT_OFFSET_T;
static float g_offsetRH = DEFAULT_OFFSET_RH;
static float g_offsetM  = DEFAULT_OFFSET_M;

// Estado para DR (M de la publicación anterior).
static float g_prevM = -1.0f;

// Cadencia de publicación (default §4; el aprovisionamiento la ajusta vía intervaloEnvioSegundos).
static uint32_t g_publishMs = (uint32_t)PUBLISH_INTERVAL_S * 1000UL;
static uint32_t g_lastPublish = 0;
static bool     g_firstPublish = true;  // publica pronto al arrancar, luego cada g_publishMs

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

// Consume el aprovisionamiento del backend (shared attributes de ThingsBoard, Etapa 4a):
// llena el lote y los offsets/cadencia vigentes.
static void onAttributes(const char* json, unsigned int len) {
  applyAttributes(json, len, g_lot, g_offsetT, g_offsetRH, g_publishMs);
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

  // ETAPA 4a: sin lote hardcodeado. El lote llega por aprovisionamiento (shared attributes).
  netSetAttributeHandler(onAttributes);
  netBegin();                  // conecta WiFi (MQTT se conecta en el loop)
  provisioningSyncTime(8000);  // NTP antes del 1er MQTT connect -> mapea loteInicioTs a t real
}

void loop() {
  netLoop();  // mantiene WiFi/MQTT vivos (no bloqueante)

  uint32_t now = millis();
  if (!g_firstPublish && (now - g_lastPublish < g_publishMs)) { delay(20); return; }
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

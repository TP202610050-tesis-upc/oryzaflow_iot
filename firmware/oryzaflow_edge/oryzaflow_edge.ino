// ============================================================================
// OryzaFlow edge — firmware (Heltec WiFi LoRa 32 V3 / ESP32-S3)
// Sensores -> modelo de secado -> JSON canónico -> ThingsBoard (WiFi/MQTT).
// ETAPA 4a: el lote lo aprovisiona el backend por shared attributes (sin hardcode).
// v1.1.0 (docs/fallos_firmware.md): Chung-Pfost en base húmeda (F1/F2), integración
// por pasos con Me variable (F10), estado por segmento diario (F3), om/seg (F8/F3), Midilli (F4).
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

// Parámetros del lote/segmento: los aprovisiona el backend por shared attributes (Etapa 4a).
// Arranca INACTIVO -> el edge publica solo tipo A hasta que llegue un lote.
static LotParams g_lot;

// Offsets de calibración vigentes (los sobreescribirá el aprovisionamiento).
static float g_offsetT  = DEFAULT_OFFSET_T;
static float g_offsetRH = DEFAULT_OFFSET_RH;

// Estado de la integración por pasos (M_prev, reloj del ciclo anterior). Se reinicia al cambiar
// de lote/segmento (g_lot.generation), de modo que el primer dr del segmento se omite (F3).
static ModelState g_model;
static uint32_t   g_lotGeneration = 0;

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

// ---------------------------------------------------------------------------
// Auto-test determinista del modelo (verificable a mano; mismos casos que tools/replay_model.py --self-check).
// Lote factible: M0=22, M_target=13 (% b.h.), k=0.12, n=1.05; ambiente T=42 °C, HR=45 %.
// Chung-Pfost a 42/45: Me_bs=10.40 -> Me_bh=9.42.  Esperado @t=5h (forma cerrada):
//   mr = exp(-0.12*5^1.05) = 0.5219;  m = 9.42 + (22-9.42)*0.5219 = 15.99;
//   MR_obj = (13-9.42)/(22-9.42) = 0.2846 -> t_obj = 9.36 h -> te = 4.36 h;  dr(4h->5h) = 0.96 %/h.
// ---------------------------------------------------------------------------
static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static void selfTestModel() {
  Serial.println("--- self-test modelo v1.1.0 (F1/F2/F10/F4) ---");
  int fails = 0;

  // (a) Chung-Pfost en base humeda: criterios de aceptacion de F1/F2.
  float me1 = equilibrium_moisture_bh(24.98f, 66.20f);   // payload real HANDOFF §2 -> 12.72
  float me2 = equilibrium_moisture_bh(28.0f, 60.0f);     // 11.79 b.h. (13.37 b.s.)
  float bs2 = equilibrium_moisture_bs(28.0f, 60.0f);
  bool okA = near(me1, 12.72f, 0.01f) && near(me2, 11.79f, 0.01f) && near(bs2, 13.37f, 0.01f);
  Serial.printf("  [%s] (a) CP: me(24.98,66.2)=%.2f (esp 12.72)  me(28,60)=%.2f b.h. / %.2f b.s. (esp 11.79 / 13.37)\n",
                okA ? "PASS" : "FAIL", me1, me2, bs2);
  if (!okA) fails++;

  LotParams lp;
  lp.active = true; lp.startMs = 0; lp.segmento = 1;
  lp.m0 = 22.0f; lp.mTarget = 13.0f; lp.k = 0.12f; lp.n = 1.05f;
  char buf[256];

  // (b) M(t=0) = M0 exactamente (F2: misma base arriba y abajo).
  ModelState st0; resetModelState(st0);
  DryingState z = computeDryingState(lp, 42.0f, 45.0f, false, 0.0, st0);
  bool okB = near(z.m, 22.0f, 0.005f);
  Serial.printf("  [%s] (b) M(t=0)=%.2f (esp 22.00)\n", okB ? "PASS" : "FAIL", z.m);
  if (!okB) fails++;

  // (c) Forma cerrada (primer ciclo @4h) y paso de 1h (@5h) vs valores esperados; luego 12 pasos de 5 min.
  ModelState st1; resetModelState(st1);
  DryingState a = computeDryingState(lp, 42.0f, 45.0f, false, 4.0 * 3600000.0, st1);
  buildTelemetryJson(buf, sizeof(buf), a, 0);
  Serial.printf("      t=4h: %s\n", buf);
  DryingState b = computeDryingState(lp, 42.0f, 45.0f, false, 5.0 * 3600000.0, st1);
  buildTelemetryJson(buf, sizeof(buf), b, 0);
  Serial.printf("      t=5h: %s\n", buf);
  ModelState st2; resetModelState(st2);
  DryingState c = computeDryingState(lp, 42.0f, 45.0f, false, 4.0 * 3600000.0, st2);
  for (int i = 1; i <= 12; i++)
    c = computeDryingState(lp, 42.0f, 45.0f, false, (4.0 + i / 12.0) * 3600000.0, st2);
  bool okC = near(b.me, 9.42f, 0.01f) && near(b.mr, 0.522f, 0.001f) && near(b.m, 15.99f, 0.01f)
          && near(b.te, 4.36f, 0.01f) && b.hasDr && near(b.dr, 0.96f, 0.01f) && !a.hasDr
          && near(c.m, b.m, 0.01f);
  Serial.printf("  [%s] (c) @5h me=%.2f mr=%.4f m=%.2f te=%.2f dr=%.2f (esp 9.42 0.522 15.99 4.36 0.96); 12x5min m=%.4f (|d|=%.4f)\n",
                okC ? "PASS" : "FAIL", b.me, b.mr, b.m, b.te, b.dr, c.m, fabsf(c.m - b.m));
  if (!okC) fails++;

  // (d) Midilli con a=1, b=0 reproduce Page exactamente (criterio F4).
  LotParams lm = lp; lm.model = DryingModelType::MIDILLI; lm.a = 1.0f; lm.b = 0.0f;
  ModelState st3; resetModelState(st3);
  DryingState d = computeDryingState(lm, 42.0f, 45.0f, false, 4.0 * 3600000.0, st3);
  d = computeDryingState(lm, 42.0f, 45.0f, false, 5.0 * 3600000.0, st3);
  bool okD = (d.m == b.m) && (d.te == b.te) && (d.mr == b.mr);
  Serial.printf("  [%s] (d) Midilli(a=1,b=0) m=%.4f te=%.4f == Page m=%.4f te=%.4f\n",
                okD ? "PASS" : "FAIL", d.m, d.te, b.m, b.te);
  if (!okD) fails++;

  Serial.printf("--- fin self-test: %s (%d fallos) ---\n", fails == 0 ? "PASS" : "FAIL", fails);
}

// Consume el aprovisionamiento del backend (shared attributes de ThingsBoard, Etapa 4a):
// llena el lote/segmento y los offsets/cadencia vigentes.
static void onAttributes(const char* json, unsigned int len) {
  applyAttributes(json, len, g_lot, g_offsetT, g_offsetRH, g_publishMs);
  netSetPublishIntervalS(g_publishMs / 1000UL);
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(400);
  Serial.println("\n=== OryzaFlow edge v" FW_VERSION " ===");

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
  g_lastPublish = now;

  // F3: nuevo lote/segmento (o fin de lote) -> se descarta M_prev; el primer dr del segmento se omite.
  if (g_lot.generation != g_lotGeneration) {
    g_lotGeneration = g_lot.generation;
    resetModelState(g_model);
    Serial.printf("[model] reset de estado (gen=%lu, seg=%u)\n", (unsigned long)g_lotGeneration, (unsigned)g_lot.segmento);
  }

  Sht31Reading s = sht.read();
  if (!s.ok) { Serial.println("{\"err\":\"sht31\"}"); return; }   // el estado no avanza: el proximo dt cubre el hueco
  float t  = s.tempC  + g_offsetT;
  float rh = s.humPct + g_offsetRH;
  bool  rain = rainDetected();

  DryingState st = computeDryingState(g_lot, t, rh, rain, (double)now, g_model);

  char json[256];
  buildTelemetryJson(json, sizeof(json), st, 0);  // sin ts: ThingsBoard sella
  Serial.println(json);

  bool pub = netPublish(json);
  Serial.printf("[mqtt] publish=%s estado=%s\n", pub ? "ok" : "NO", netStatus());

  displayRender(st, netStatus());
}

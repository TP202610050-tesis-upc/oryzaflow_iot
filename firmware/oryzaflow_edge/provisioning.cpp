#include "provisioning.h"
#include "config.h"
#include <ArduinoJson.h>
#include <time.h>

// Reloj válido si supera ~2023-11-14 (descarta el epoch 0 de antes de sincronizar NTP).
static const time_t TS_VALID_MIN = 1700000000;

// Epoch actual en ms (UTC). 0 si NTP aún no sincronizó.
static double nowEpochMs() {
  time_t now = time(nullptr);
  if (now < TS_VALID_MIN) return 0;
  return (double)now * 1000.0;
}

bool provisioningSyncTime(uint32_t timeoutMs) {
  configTime(0, 0, NTP_SERVER);  // UTC: loteInicioTs del backend es epoch UTC (Date.getTime)
  Serial.print("[ntp] sincronizando");
  uint32_t t0 = millis();
  while (nowEpochMs() == 0 && millis() - t0 < timeoutMs) { delay(250); Serial.print("."); }
  bool ok = nowEpochMs() != 0;
  Serial.println(ok ? " OK" : " (sin NTP: t=0 sera el arranque)");
  return ok;
}

// startMs en escala millis() tal que (millis()-startMs) == tiempo real desde loteInicioTs.
// Si no hay NTP, cae a millis() (t arranca ahora).
static double resolveStartMs(double loteInicioTsMs) {
  double epoch = nowEpochMs();
  if (epoch == 0) return (double)millis();
  return (double)millis() - (epoch - loteInicioTsMs);
}

static void applyObject(JsonObjectConst o, LotParams& lot, float& offsetT, float& offsetRH,
                        uint32_t& publishMs) {
  if (!o["loteId"].isNull()) {
    lot.active = true;
    Serial.printf("[prov] lote activo: %s\n", o["loteId"].as<const char*>());
  }
  if (!o["loteInicioTs"].isNull()) lot.startMs = resolveStartMs(o["loteInicioTs"].as<double>());
  if (!o["m0"].isNull())       lot.m0      = o["m0"].as<float>();
  if (!o["mTarget"].isNull())  lot.mTarget = o["mTarget"].as<float>();
  if (!o["k"].isNull())        lot.k       = o["k"].as<float>();
  if (!o["n"].isNull())        lot.n       = o["n"].as<float>();
  if (!o["offsetM"].isNull())  lot.offsetM = o["offsetM"].as<float>();
  if (!o["offsetT"].isNull())  offsetT     = o["offsetT"].as<float>();
  if (!o["offsetRh"].isNull()) offsetRH    = o["offsetRh"].as<float>();
  if (!o["intervaloEnvioSegundos"].isNull()) {
    uint32_t s = o["intervaloEnvioSegundos"].as<uint32_t>();
    if (s > 0) publishMs = s * 1000UL;
  }
}

void applyAttributes(const char* json, unsigned int len,
                     LotParams& lot, float& offsetT, float& offsetRH, uint32_t& publishMs) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) { Serial.printf("[prov] JSON invalido: %s\n", err.c_str()); return; }

  // Borrado de claves (fin de lote): {"deleted":["loteId",...]} -> el edge vuelve a tipo A.
  if (doc["deleted"].is<JsonArrayConst>()) {
    for (JsonVariantConst v : doc["deleted"].as<JsonArrayConst>()) {
      const char* key = v.as<const char*>();
      if (key && strcmp(key, "loteId") == 0) {
        lot.active = false;
        Serial.println("[prov] lote finalizado -> tipo A");
      }
    }
    return;
  }

  // Respuesta a request: {"shared":{...}}.  Push de cambios: {...} directo.
  if (doc["shared"].is<JsonObjectConst>())
    applyObject(doc["shared"].as<JsonObjectConst>(), lot, offsetT, offsetRH, publishMs);
  else
    applyObject(doc.as<JsonObjectConst>(), lot, offsetT, offsetRH, publishMs);
}

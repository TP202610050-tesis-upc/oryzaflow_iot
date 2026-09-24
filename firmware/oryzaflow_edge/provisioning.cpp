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

// Actualiza un float y devuelve true si cambió.
static bool setF(float& dst, JsonVariantConst v) {
  float x = v.as<float>();
  if (x == dst) return false;
  dst = x;
  return true;
}

static void applyObject(JsonObjectConst o, LotParams& lot, float& offsetT, float& offsetRH,
                        uint32_t& publishMs) {
  bool segmentChanged = false;   // algo que exige reiniciar el estado del modelo (F3)

  JsonVariantConst lid = o["loteId"];
  if (!lid.isUnbound()) {
    if (lid.isNull()) {
      // {"loteId": null} explícito (ESQUEMA §3): fin de lote.
      if (lot.active) { lot.active = false; segmentChanged = true; Serial.println("[prov] loteId=null -> tipo A"); }
    } else {
      if (!lot.active) segmentChanged = true;
      lot.active = true;
      Serial.printf("[prov] lote activo: %s\n", lid.as<const char*>());
    }
  }
  if (!o["loteInicioTs"].isNull()) {
    double raw = o["loteInicioTs"].as<double>();
    if (raw != lot.inicioTsRaw) {
      lot.inicioTsRaw = raw;
      lot.startMs = resolveStartMs(raw);
      segmentChanged = true;
    }
  }
  if (!o["m0"].isNull())       segmentChanged |= setF(lot.m0, o["m0"]);
  if (!o["mTarget"].isNull())  setF(lot.mTarget, o["mTarget"]);
  if (!o["k"].isNull())        setF(lot.k, o["k"]);
  if (!o["n"].isNull())        setF(lot.n, o["n"]);
  if (!o["a"].isNull())        setF(lot.a, o["a"]);
  if (!o["b"].isNull())        setF(lot.b, o["b"]);
  if (!o["segmento"].isNull()) {
    uint16_t seg = o["segmento"].as<uint16_t>();
    if (seg != lot.segmento) { lot.segmento = seg; segmentChanged = true; }
  }
  if (!o["modelo"].isNull()) {
    const char* m = o["modelo"].as<const char*>();
    DryingModelType t = (m && strcmp(m, "MIDILLI") == 0) ? DryingModelType::MIDILLI : DryingModelType::PAGE;
    if (m && strcmp(m, "MIDILLI") != 0 && strcmp(m, "PAGE") != 0)
      Serial.printf("[prov] modelo desconocido '%s' -> PAGE\n", m);
    lot.model = t;
  }
  if (!o["meModel"].isNull()) {
    // F1: alternativas (Henderson modificada, Steffe-Singh) pendientes de constantes citadas (TODO(fuente)).
    const char* mm = o["meModel"].as<const char*>();
    if (mm && strcmp(mm, "chung_pfost") != 0)
      Serial.printf("[prov] meModel '%s' no implementado; se usa chung_pfost\n", mm);
  }
  if (!o["offsetM"].isNull())  setF(lot.offsetM, o["offsetM"]);
  if (!o["offsetT"].isNull())  setF(offsetT, o["offsetT"]);
  if (!o["offsetRh"].isNull()) setF(offsetRH, o["offsetRh"]);
  if (!o["intervaloEnvioSegundos"].isNull()) {
    uint32_t s = o["intervaloEnvioSegundos"].as<uint32_t>();
    if (s > 0) publishMs = s * 1000UL;
  }

  if (segmentChanged) {
    lot.generation++;
    Serial.printf("[prov] lote/segmento cambiado (gen=%lu, seg=%u, m0=%.2f)\n",
                  (unsigned long)lot.generation, (unsigned)lot.segmento, lot.m0);
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
      if (key && strcmp(key, "loteId") == 0 && lot.active) {
        lot.active = false;
        lot.generation++;
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

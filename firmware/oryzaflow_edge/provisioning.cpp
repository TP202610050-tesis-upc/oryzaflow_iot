#include "provisioning.h"
#include "config.h"
#include <ArduinoJson.h>
#include <time.h>

// Reloj válido si supera ~2023-11-14 (descarta el epoch 0 de antes de sincronizar).
static const uint32_t TS_VALID_MIN = 1700000000UL;

// Ancla del reloj fijado por la red (LoRaWAN) o por TIME_HINT: epoch ms en el instante millis()==s_anchorMs.
static double   s_epochMsAtAnchor = 0;
static uint32_t s_anchorMs = 0;

double clockNowEpochMs() {
#if TRANSPORT_MODE == TRANSPORT_WIFI
  time_t now = time(nullptr);
  if ((uint32_t)now >= TS_VALID_MIN) return (double)now * 1000.0;
#endif
  if (s_epochMsAtAnchor > 0) return s_epochMsAtAnchor + (double)(millis() - s_anchorMs);
  return 0;
}

void clockSetEpochSeconds(uint32_t epochSeconds) {
  if (epochSeconds < TS_VALID_MIN) { Serial.printf("[clock] epoch invalido ignorado: %lu\n", (unsigned long)epochSeconds); return; }
  s_epochMsAtAnchor = (double)epochSeconds * 1000.0;
  s_anchorMs = millis();
  Serial.printf("[clock] hora fijada: epoch=%lu\n", (unsigned long)epochSeconds);
}

bool provisioningSyncTime(uint32_t timeoutMs) {
#if TRANSPORT_MODE == TRANSPORT_WIFI
  configTime(0, 0, NTP_SERVER);  // UTC: loteInicioTs del backend es epoch UTC (Date.getTime)
  Serial.print("[ntp] sincronizando");
  uint32_t t0 = millis();
  while (clockNowEpochMs() == 0 && millis() - t0 < timeoutMs) { delay(250); Serial.print("."); }
  bool ok = clockNowEpochMs() != 0;
  Serial.println(ok ? " OK" : " (sin NTP: t=0 sera el arranque)");
  return ok;
#else
  (void)timeoutMs;
  return clockNowEpochMs() != 0;
#endif
}

// startMs en escala millis() tal que (millis()-startMs) == tiempo real desde loteInicioTs.
// Sin reloj válido, cae a millis() (t arranca ahora).
static double resolveStartMs(double loteInicioTsMs) {
  double epoch = clockNowEpochMs();
  if (epoch == 0) return (double)millis();
  return (double)millis() - (epoch - loteInicioTsMs);
}

void provisioningOnClockSync(LotParams& lot) {
  if (lot.active && lot.inicioTsRaw > 0) {
    lot.startMs = resolveStartMs(lot.inicioTsRaw);
    Serial.println("[prov] startMs re-anclado con la hora sincronizada");
  }
}

// Actualiza un float y devuelve true si cambió.
static bool setF(float& dst, float x) {
  if (x == dst) return false;
  dst = x;
  return true;
}

static void logSegmentChange(const LotParams& lot) {
  Serial.printf("[prov] lote/segmento cambiado (gen=%lu, seg=%u, m0=%.2f)\n",
                (unsigned long)lot.generation, (unsigned)lot.segmento, lot.m0);
}

// ============================================================================ WIFI: shared attributes JSON
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
  if (!o["m0"].isNull())       segmentChanged |= setF(lot.m0, o["m0"].as<float>());
  if (!o["mTarget"].isNull())  setF(lot.mTarget, o["mTarget"].as<float>());
  if (!o["k"].isNull())        setF(lot.k, o["k"].as<float>());
  if (!o["n"].isNull())        setF(lot.n, o["n"].as<float>());
  if (!o["a"].isNull())        setF(lot.a, o["a"].as<float>());
  if (!o["b"].isNull())        setF(lot.b, o["b"].as<float>());
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
  if (!o["offsetM"].isNull())  setF(lot.offsetM, o["offsetM"].as<float>());
  if (!o["offsetT"].isNull())  setF(offsetT, o["offsetT"].as<float>());
  if (!o["offsetRh"].isNull()) setF(offsetRH, o["offsetRh"].as<float>());
  if (!o["intervaloEnvioSegundos"].isNull()) {
    uint32_t s = o["intervaloEnvioSegundos"].as<uint32_t>();
    if (s > 0) publishMs = s * 1000UL;
  }

  if (segmentChanged) { lot.generation++; logSegmentChange(lot); }
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

// ============================================================================ LORAWAN: downlink binario (§5.4)
static inline uint16_t rdU16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline int16_t  rdI16(const uint8_t* p) { return (int16_t)rdU16(p); }
static inline uint32_t rdU32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

void applyDownlink(uint8_t port, const uint8_t* b, size_t n,
                   LotParams& lot, float& offsetT, float& offsetRH, uint32_t& publishMs) {
  if (port != LORAWAN_DOWNLINK_PORT || n < 1) { Serial.printf("[prov] downlink ignorado (puerto %u, %u B)\n", port, (unsigned)n); return; }
  switch (b[0]) {
    case 0x01: {   // SET_LOT: inicioTs u32 · m0 u16×100 · mTarget u16×100 · k u16×10000 · n u16×1000 · modelo u8 · a u16×1000 · b i16×10000 · offsetM i16×100 · segmento u8 · [lotId u16]
      if (n < 21) { Serial.printf("[prov] SET_LOT corto (%u B)\n", (unsigned)n); return; }
      bool changed = !lot.active;
      double inicioTsMs = (double)rdU32(b + 1) * 1000.0;
      if (inicioTsMs != lot.inicioTsRaw) { lot.inicioTsRaw = inicioTsMs; lot.startMs = resolveStartMs(inicioTsMs); changed = true; }
      changed |= setF(lot.m0, rdU16(b + 5) / 100.0f);
      setF(lot.mTarget, rdU16(b + 7) / 100.0f);
      setF(lot.k, rdU16(b + 9) / 10000.0f);
      setF(lot.n, rdU16(b + 11) / 1000.0f);
      lot.model = (b[13] == 1) ? DryingModelType::MIDILLI : DryingModelType::PAGE;
      setF(lot.a, rdU16(b + 14) / 1000.0f);
      setF(lot.b, rdI16(b + 16) / 10000.0f);
      setF(lot.offsetM, rdI16(b + 18) / 100.0f);
      uint16_t seg = b[20];
      if (seg != lot.segmento) { lot.segmento = seg; changed = true; }
      lot.active = true;
      Serial.printf("[prov] SET_LOT lote=%u seg=%u m0=%.2f mTarget=%.2f k=%.4f n=%.3f %s a=%.3f b=%.4f offsetM=%.2f inicioTs=%lu\n",
                    (unsigned)(n >= 23 ? rdU16(b + 21) : 0), (unsigned)lot.segmento, lot.m0, lot.mTarget, lot.k, lot.n,
                    lot.model == DryingModelType::MIDILLI ? "MIDILLI" : "PAGE", lot.a, lot.b, lot.offsetM,
                    (unsigned long)rdU32(b + 1));
      if (changed) { lot.generation++; logSegmentChange(lot); }
      break;
    }
    case 0x02: {   // CLEAR_LOT
      if (lot.active) { lot.active = false; lot.generation++; Serial.println("[prov] CLEAR_LOT -> tipo A"); }
      break;
    }
    case 0x03: {   // SET_NODE: offsetT i16×100 · offsetRh i16×100 · intervaloEnvioSegundos u16
      if (n < 7) return;
      setF(offsetT, rdI16(b + 1) / 100.0f);
      setF(offsetRH, rdI16(b + 3) / 100.0f);
      uint16_t s = rdU16(b + 5);
      if (s > 0) publishMs = (uint32_t)s * 1000UL;
      Serial.printf("[prov] SET_NODE offsetT=%.2f offsetRh=%.2f intervalo=%us\n", offsetT, offsetRH, (unsigned)s);
      break;
    }
    case 0x04: {   // TIME_HINT: epoch u32 (respaldo si la red no responde DeviceTimeReq)
      if (n < 5) return;
      clockSetEpochSeconds(rdU32(b + 1));
      provisioningOnClockSync(lot);
      break;
    }
    default:
      Serial.printf("[prov] comando de downlink desconocido 0x%02X\n", b[0]);
  }
}

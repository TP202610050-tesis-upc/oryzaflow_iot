#pragma once
// Codec binario del JSON canónico (ESQUEMA_TELEMETRIA.md §5, v1.1) para el transporte LoRaWAN.
// Big-endian, sin padding. decode() de referencia: lorawan/ttn_uplink_formatter.js (The Things Stack).
//
//   [0]     flags   bit0 lot · bit1 rain · bit2 hasDr · bit3 hasTe · bit4 fault · bit5 hasSeg · bit6 hasOm
//   [1..2]  t   int16  ×100 (°C)
//   [3..4]  rh  uint16 ×100 (%)
//   [5..6]  me  uint16 ×100 (% b.h.)
//   si lot: m uint16 ×100 · mr uint16 ×10000 · [dr int16 ×100] · [te uint16 ×100] · [seg uint8] · [om int16 ×100]
//   Longitudes: 1 B (solo fault: sensor sin lectura, latido) · 7 B (sin lote) … 18 B (lote completo).
#include <Arduino.h>
#include "drying_model.h"

#define TELEMETRY_CODEC_MAX_BYTES 18

// Serializa s en out (cap >= TELEMETRY_CODEC_MAX_BYTES). Devuelve los bytes escritos.
size_t encodeTelemetry(const DryingState& s, uint8_t* out, size_t cap);

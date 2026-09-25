#include "codec.h"
#include <math.h>

static inline int32_t scaled(float v, float k) {
  if (isnan(v)) return 0;
  return (int32_t)lroundf(v * k);
}
static inline void putU16(uint8_t* p, uint32_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
static inline void putI16(uint8_t* p, int32_t v) {
  if (v < -32768) v = -32768;
  if (v > 32767)  v = 32767;
  putU16(p, (uint16_t)(v & 0xFFFF));
}
static inline uint32_t clampU16(int32_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : (uint32_t)v); }

size_t encodeTelemetry(const DryingState& s, uint8_t* out, size_t cap) {
  if (cap < TELEMETRY_CODEC_MAX_BYTES) return 0;
  uint8_t flags = 0;
  if (s.hasB)  flags |= 0x01;
  if (s.rain)  flags |= 0x02;
  if (s.hasB && s.hasDr) flags |= 0x04;
  if (s.hasB)  flags |= 0x08;                 // te siempre presente con lote
  if (isnan(s.tC) || isnan(s.rhPct)) flags |= 0x10;
  if (s.hasB && s.seg > 0) flags |= 0x20;
  if (s.hasB)  flags |= 0x40;                 // om siempre presente con lote (auditoría F8)

  size_t o = 0;
  out[o++] = flags;
  putI16(out + o, scaled(s.tC, 100.0f));    o += 2;
  putU16(out + o, clampU16(scaled(s.rhPct, 100.0f))); o += 2;
  putU16(out + o, clampU16(scaled(s.me, 100.0f)));    o += 2;
  if (s.hasB) {
    putU16(out + o, clampU16(scaled(s.m, 100.0f)));    o += 2;
    putU16(out + o, clampU16(scaled(s.mr, 10000.0f))); o += 2;
    if (s.hasDr) { putI16(out + o, scaled(s.dr, 100.0f)); o += 2; }
    putU16(out + o, clampU16(scaled(s.te, 100.0f)));   o += 2;
    if (s.seg > 0) out[o++] = (uint8_t)(s.seg > 255 ? 255 : s.seg);
    putI16(out + o, scaled(s.om, 100.0f));             o += 2;
  }
  return o;
}

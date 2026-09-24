#include "telemetry.h"

size_t buildTelemetryJson(char* buf, size_t cap, const DryingState& s, double tsMs) {
  size_t len = 0;
  len += snprintf(buf + len, cap - len, "{");
  if (tsMs > 0)
    len += snprintf(buf + len, cap - len, "\"ts\":%.0f,", tsMs);

  // Siempre: t, rh, rain, me, lot
  len += snprintf(buf + len, cap - len,
                  "\"t\":%.2f,\"rh\":%.2f,\"rain\":%s,\"me\":%.2f,\"lot\":%s",
                  s.tC, s.rhPct, s.rain ? "true" : "false", s.me,
                  s.hasB ? "true" : "false");

  // Tipo B: m, mr (siempre que haya lote), dr (si hay M previa), te, om (auditoria F8), seg (F3, si > 0)
  if (s.hasB) {
    len += snprintf(buf + len, cap - len, ",\"m\":%.2f,\"mr\":%.4f", s.m, s.mr);
    if (s.hasDr)
      len += snprintf(buf + len, cap - len, ",\"dr\":%.2f", s.dr);
    len += snprintf(buf + len, cap - len, ",\"te\":%.2f,\"om\":%.2f", s.te, s.om);
    if (s.seg > 0)
      len += snprintf(buf + len, cap - len, ",\"seg\":%u", (unsigned)s.seg);
  }

  len += snprintf(buf + len, cap - len, "}");
  return len;
}

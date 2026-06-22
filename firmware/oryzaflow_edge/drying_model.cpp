#include "drying_model.h"
#include "config.h"
#include <math.h>

float henderson_me(float tC, float rhPct) {
  float hr = rhPct / 100.0f;
  // Guardas numéricas: ln(1-HR) explota cerca de HR=1.
  if (hr < 0.0001f) hr = 0.0001f;
  if (hr > 0.999f)  hr = 0.999f;
  float num = -logf(1.0f - hr);
  float den = HENDERSON_A * (tC + HENDERSON_C);
  if (den <= 0) return NAN;
  return powf(num / den, 1.0f / HENDERSON_B);
}

DryingState computeDryingState(const LotParams& lp, float tC, float rhPct, bool rain,
                               double nowMs, float prevM, float dtHours) {
  DryingState s;
  s.tC = tC;
  s.rhPct = rhPct;
  s.rain = rain;
  s.me = henderson_me(tC, rhPct);

  if (!lp.active) return s;  // sin lote -> solo tipo A

  // Tiempo transcurrido del lote en horas (con escala de demo opcional).
  float tH = (float)((nowMs - lp.startMs) / 3600000.0) * MODEL_TIME_SCALE;
  if (tH < 0) tH = 0;

  // Page: MR(t) = exp(-k * t^n); M(t) = Me + (M0-Me)*MR
  float mr = expf(-lp.k * powf(tH, lp.n));
  float m  = s.me + (lp.m0 - s.me) * mr + lp.offsetM;
  s.mr = mr;
  s.m  = m;
  s.hasB = true;

  // Tiempo restante estimado hasta M_target (informativo).
  float denom = (lp.m0 - s.me);
  float mrObj = (denom != 0.0f) ? (lp.mTarget - s.me) / denom : -1.0f;
  if (mrObj > 0.0f && mrObj < 1.0f) {
    float tTarget = powf(-logf(mrObj) / lp.k, 1.0f / lp.n);
    float te = tTarget - tH;
    s.te = (te > 0) ? te : 0.0f;
  } else {
    // mrObj<=0 -> Me>=M_target (meta inviable con el aire actual);
    // mrObj>=1 -> M_target>=M0 (ya cumple). En ambos, restante = 0.
    s.te = 0.0f;
  }

  // DR entre publicaciones consecutivas.
  if (prevM >= 0 && dtHours > 0) {
    s.dr = (prevM - m) / dtHours;
    s.hasDr = true;
  }
  return s;
}

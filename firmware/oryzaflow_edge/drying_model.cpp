#include "drying_model.h"
#include "config.h"
#include <math.h>

// NOTA: tools/replay_model.py reproduce ESTE archivo operacion por operacion (float32). Cualquier cambio
// aqui debe replicarse alli para que la prueba de equivalencia nodo <-> offline (F6) siga valiendo.

float moisture_bs_to_bh(float bs) { return 100.0f * bs / (100.0f + bs); }
float moisture_bh_to_bs(float bh) { return 100.0f * bh / (100.0f - bh); }

float equilibrium_moisture_bs(float tC, float rhPct) {
  float hr = rhPct / 100.0f;
  // Guardas numericas: ln(HR) explota en 0 y vale 0 en 1.
  if (hr < 0.01f) hr = 0.01f;
  if (hr > 0.99f) hr = 0.99f;
  float arg = -(tC + CP_C) * logf(hr);
  if (arg <= 0.0f) return NAN;
  return CP_E - CP_F * logf(arg);
}

float equilibrium_moisture_bh(float tC, float rhPct) {
  float bs = equilibrium_moisture_bs(tC, rhPct);
  if (isnan(bs)) return NAN;
  return moisture_bs_to_bh(bs);
}

float model_mr(const LotParams& lp, float tH) {
  if (tH < 0.0f) tH = 0.0f;
  float page = expf(-lp.k * powf(tH, lp.n));
  if (lp.model == DryingModelType::MIDILLI) return lp.a * page + lp.b * tH;
  return page;
}

static bool isPageEquivalent(const LotParams& lp) {
  return lp.model == DryingModelType::PAGE || (lp.a == 1.0f && lp.b == 0.0f);
}

float model_time_for_mr(const LotParams& lp, float mr) {
  if (isPageEquivalent(lp)) {
    // Page: t = (-ln(MR)/k)^(1/n). Con a=1,b=0 Midilli es identica (criterio F4).
    if (mr >= 1.0f) return 0.0f;
    if (mr <= 0.0f) return NAN;
    return powf(-logf(mr) / lp.k, 1.0f / lp.n);
  }
  // Midilli general: no hay forma cerrada -> biseccion sobre una MR(t) que se asume decreciente en [0, T_MAX].
  if (mr >= lp.a) return 0.0f;
  float lo = 0.0f, hi = MODEL_T_MAX_H;
  if (model_mr(lp, hi) > mr) return hi;   // no alcanza mr dentro del horizonte
  for (int i = 0; i < 40; i++) {
    float mid = 0.5f * (lo + hi);
    if (model_mr(lp, mid) > mr) lo = mid; else hi = mid;
  }
  return 0.5f * (lo + hi);
}

void resetModelState(ModelState& ms) {
  ms.valid = false;
  ms.mPrev = NAN;
  ms.tEff = 0.0f;
  ms.prevMs = 0;
}

DryingState computeDryingState(const LotParams& lp, float tC, float rhPct, bool rain,
                               double nowMs, ModelState& ms) {
  DryingState s;
  s.tC = tC;
  s.rhPct = rhPct;
  s.rain = rain;
  s.me = equilibrium_moisture_bh(tC, rhPct);   // tipo A, siempre (b.h.)

  if (!lp.active) return s;                              // sin lote -> solo tipo A
  if (!(lp.k > 0.0f) || !(lp.n > 0.0f) || isnan(s.me)) return s;   // parametros invalidos -> solo tipo A

  const float me = s.me;
  const float denom = lp.m0 - me;   // potencial de secado; <= 0 => el aire no puede secar el grano
  float mModel;                     // M del modelo sin offset
  float tEff;                       // tiempo equivalente efectivo tras este ciclo [h]
  bool  drying = false;             // hubo secado en este paso (para te)

  if (!ms.valid) {
    // Primer ciclo del segmento (o tras reinicio a mitad de segmento): unica opcion sin historial es la
    // forma cerrada con el tiempo transcurrido desde loteInicioTs y la Me actual.
    float tH = (float)((nowMs - lp.startMs) / 3600000.0) * MODEL_TIME_SCALE;
    if (tH < 0.0f) tH = 0.0f;
    if (denom <= MODEL_EPS) {
      mModel = lp.m0;
      tEff = 0.0f;
    } else {
      mModel = me + denom * model_mr(lp, tH);
      tEff = tH;
      drying = true;
    }
  } else {
    float dt = (float)((nowMs - ms.prevMs) / 3600000.0) * MODEL_TIME_SCALE;
    if (dt < 0.0f) dt = 0.0f;
    float mrPrev = (denom > MODEL_EPS) ? (ms.mPrev - me) / denom : 0.0f;
    if (mrPrev <= 0.0f) {
      // El grano ya esta en/bajo el equilibrio actual (tarde/noche): sin secado, sin readsorcion.
      mModel = ms.mPrev;
      tEff = ms.tEff;
    } else {
      if (mrPrev > 1.0f) mrPrev = 1.0f;
      if (mrPrev < MODEL_MR_MIN) mrPrev = MODEL_MR_MIN;
      float tEq = model_time_for_mr(lp, mrPrev);   // tiempo equivalente con la Me de este ciclo
      tEff = tEq + dt;
      mModel = me + denom * model_mr(lp, tEff);
      drying = true;
    }
    if (dt > 0.0f) {
      s.dr = (ms.mPrev - mModel) / dt;   // el offset se cancela; DR es del modelo
      s.hasDr = true;
    }
  }

  // Razon de humedad actual respecto a la Me de este ciclo (adimensional, misma base arriba y abajo).
  float mr = (denom > MODEL_EPS) ? (mModel - me) / denom : 1.0f;
  if (mr < 0.0f) mr = 0.0f;
  if (mr > 1.0f) mr = 1.0f;
  s.mr = mr;
  s.m = mModel + lp.offsetM;
  s.hasB = true;

  // Tiempo restante hasta M_target desde la posicion actual (t_eff), con la Me actual. Informativo.
  float mrObj = (denom > MODEL_EPS) ? (lp.mTarget - me) / denom : -1.0f;
  if (drying && mrObj > 0.0f && mrObj < 1.0f && mModel > lp.mTarget) {
    float te = model_time_for_mr(lp, mrObj) - tEff;
    s.te = (te > 0.0f) ? te : 0.0f;
  } else {
    // mrObj<=0 -> Me>=M_target (meta inviable con el aire actual); mrObj>=1 o M<=M_target -> ya cumple;
    // sin secado en el paso -> no hay avance que proyectar. En todos, restante = 0.
    s.te = 0.0f;
  }

  s.seg = lp.segmento;
  s.om  = lp.offsetM;

  ms.mPrev = mModel;
  ms.tEff = tEff;
  ms.prevMs = nowMs;
  ms.valid = true;
  return s;
}

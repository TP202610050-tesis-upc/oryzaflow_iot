#pragma once
// Modelo de secado en capa delgada (CONTEXTO_PROYECTO §7), calculado en el edge.
//   me  = Henderson modificado          (tipo A: solo ambiente, siempre)
//   m   = Page: M(t)=Me+(M0-Me)*MR      (tipo B: requiere lote aprovisionado)
//   mr  = exp(-k*t^n)
//   dr  = (M_prev - M)/Δt   [%/h]
//   te  = (-ln(MR_obj)/k)^(1/n) - t     [h], con MR_obj=(M_target-Me)/(M0-Me)
#include <Arduino.h>

// Parámetros del lote que el backend aprovisiona (shared attributes / RPC).
struct LotParams {
  bool   active = false;  // false -> el edge solo emite tipo A
  double startMs = 0;     // referencia de inicio del lote (stand-in de loteInicioTs)
  float  m0 = 0;          // M0  [% grano]
  float  mTarget = 0;     // M_target [% grano]
  float  k = 0;           // Page (ajustado en horas)
  float  n = 0;           // Page
  float  offsetM = 0;     // corrección del modelo [% grano]
};

// Estado calculado para un ciclo (alimenta el JSON canónico).
struct DryingState {
  // crudos
  float tC = NAN, rhPct = NAN;
  bool  rain = false;
  // tipo A (siempre)
  float me = NAN;
  // tipo B (solo si hasB)
  bool  hasB = false;
  float m = NAN, mr = NAN, te = NAN;
  bool  hasDr = false;
  float dr = NAN;
};

// Humedad de equilibrio (Henderson). rhPct en %, tC en °C -> Me en % grano.
float henderson_me(float tC, float rhPct);

// Calcula el estado completo. prevM<0 => no hay M previa (se omite dr).
// nowMs/dtHours: reloj y paso entre publicaciones (en ms y horas-modelo).
DryingState computeDryingState(const LotParams& lp, float tC, float rhPct, bool rain,
                               double nowMs, float prevM, float dtHours);

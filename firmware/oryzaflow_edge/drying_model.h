#pragma once
// Modelo de secado en capa delgada (CONTEXTO_PROYECTO §7; docs/fallos_firmware.md F1/F2/F10), calculado en el edge.
//
//   me  = Chung-Pfost modificada (ASAE D245.4, arroz cascara) en % base SECA, convertida a % base HUMEDA
//         (tipo A: solo ambiente, siempre).  Todas las humedades de grano de aqui en adelante son % b.h.
//   MR  = exp(-k*t^n)                         (Page)      o   a*exp(-k*t^n) + b*t   (Midilli)
//   m   = Me + (M0 - Me)*MR                   (tipo B: requiere lote aprovisionado)
//   dr  = (M_prev - M)/Δt   [%/h]
//   te  = t(MR_obj) - t_eff  [h], con MR_obj = (M_target - Me)/(M0 - Me)
//
// Integracion por pasos con tiempo equivalente (F10): Me cambia cada ciclo, asi que la forma cerrada no se
// evalua con el tiempo total del lote. En cada publicacion se busca el tiempo equivalente t_eq tal que
// MR(t_eq) = (M_prev - Me_actual)/(M0 - Me_actual) y se avanza Δt desde ahi. Si el grano ya esta por debajo
// del equilibrio actual no hay secado en el paso (no se modela readsorcion).
#include <Arduino.h>

enum class DryingModelType : uint8_t { PAGE = 0, MIDILLI = 1 };

// Parametros del lote/segmento que el backend aprovisiona (shared attributes, ESQUEMA §3).
struct LotParams {
  bool     active = false;     // false -> el edge solo emite tipo A
  double   startMs = 0;        // inicio del SEGMENTO en escala millis() (derivado de loteInicioTs)
  double   inicioTsRaw = 0;    // loteInicioTs crudo (epoch ms) para detectar cambio de segmento
  float    m0 = 0;             // M0 del segmento  [% b.h.]
  float    mTarget = 0;        // M_target         [% b.h.]
  float    k = 0;              // constante de velocidad (ajustada en horas)
  float    n = 0;              // exponente
  DryingModelType model = DryingModelType::PAGE;
  float    a = 1.0f;           // Midilli (a=1, b=0 reproduce Page)
  float    b = 0.0f;
  float    offsetM = 0;        // correccion manual del modelo [% b.h.]; debe ser 0 en campana (F8)
  uint16_t segmento = 0;       // numero de segmento diario (0 = no aprovisionado)
  uint32_t generation = 0;     // se incrementa al cambiar lote/segmento -> el .ino resetea ModelState
};

// Estado de la integracion por pasos (F10). Se reinicia al abrir un segmento (F3).
struct ModelState {
  bool   valid = false;   // false -> proximo ciclo es el primero del segmento (forma cerrada, sin dr)
  float  mPrev = NAN;     // M del modelo (sin offset) del ciclo anterior [% b.h.]
  float  tEff = 0;        // tiempo equivalente efectivo tras el ultimo ciclo [h] (informativo)
  double prevMs = 0;      // reloj (millis) del ciclo anterior
};

// Estado calculado para un ciclo (alimenta el JSON canonico).
struct DryingState {
  // crudos
  float tC = NAN, rhPct = NAN;
  bool  rain = false;
  // tipo A (siempre)
  float me = NAN;                       // [% b.h.]
  // tipo B (solo si hasB)
  bool  hasB = false;
  float m = NAN, mr = NAN, te = NAN;    // m incluye offsetM
  bool  hasDr = false;
  float dr = NAN;
  uint16_t seg = 0;                     // numero de segmento (0 -> se omite en el JSON)
  float om = 0;                         // offsetM vigente (auditoria F8)
};

// Conversion de base (F2): M_bh = 100*M_bs/(100+M_bs);  M_bs = 100*M_bh/(100-M_bh)
float moisture_bs_to_bh(float bs);
float moisture_bh_to_bs(float bh);

// Humedad de equilibrio Chung-Pfost modificada. rhPct en %, tC en °C.
float equilibrium_moisture_bs(float tC, float rhPct);   // % base seca (NAN si el argumento del ln <= 0)
float equilibrium_moisture_bh(float tC, float rhPct);   // % base humeda (la que usa el modelo)

// Razon de humedad del modelo activo a tiempo tH [h], y su inversa (tiempo al que MR(t) = mr).
float model_mr(const LotParams& lp, float tH);
float model_time_for_mr(const LotParams& lp, float mr);

void resetModelState(ModelState& ms);

// Calcula el estado completo y actualiza ms (mPrev/prevMs). nowMs en escala millis().
DryingState computeDryingState(const LotParams& lp, float tC, float rhPct, bool rain,
                               double nowMs, ModelState& ms);

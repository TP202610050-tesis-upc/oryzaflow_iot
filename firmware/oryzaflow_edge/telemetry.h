#pragma once
// Construcción del JSON canónico de telemetría (ESQUEMA_TELEMETRIA.md §2).
// Reglas de presencia: t/rh/rain/me/lot siempre; m/mr/te/om solo si lote activo;
// dr solo si además hay M previa; seg solo si el backend aprovisionó un segmento. Sin dependencias (snprintf).
#include <Arduino.h>
#include "drying_model.h"

// Serializa s en buf. tsMs>0 incluye "ts"; tsMs<=0 lo omite (ThingsBoard sella).
// Devuelve la longitud escrita (sin contar el '\0').
size_t buildTelemetryJson(char* buf, size_t cap, const DryingState& s, double tsMs);

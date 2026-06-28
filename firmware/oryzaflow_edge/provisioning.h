#pragma once
// ETAPA 4a: el edge consume el aprovisionamiento del backend (ThingsBoard shared
// attributes, ver ESQUEMA_TELEMETRIA.md §3) y llena LotParams en vez del lote
// hardcodeado. Sincroniza la hora por NTP para que el t=0 del modelo de secado sea
// el INICIO REAL del lote (loteInicioTs), no el arranque de la placa.
#include <Arduino.h>
#include "drying_model.h"

// Sincroniza la hora por NTP (requiere WiFi ya conectado). Espera hasta timeoutMs a
// que el reloj sea válido. Devuelve true si quedó sincronizado.
bool provisioningSyncTime(uint32_t timeoutMs);

// Aplica un mensaje de atributos de ThingsBoard a los parámetros vigentes. Maneja:
//  - respuesta a request:  {"shared":{...}}
//  - push de cambios:      {...}
//  - borrado (fin de lote):{"deleted":["loteId",...]}  -> lote inactivo (tipo A)
void applyAttributes(const char* json, unsigned int len,
                     LotParams& lot, float& offsetT, float& offsetRH, uint32_t& publishMs);

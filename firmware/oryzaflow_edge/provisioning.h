#pragma once
// Aprovisionamiento backend -> edge (ESQUEMA_TELEMETRIA.md §3) y reloj del nodo.
//  - Modo WIFI:    shared attributes JSON de ThingsBoard (applyAttributes) + hora por NTP.
//  - Modo LORAWAN: downlink binario en LORAWAN_DOWNLINK_PORT (applyDownlink, §5.4) + hora por DeviceTimeReq
//                  (o comando TIME_HINT). Sin reloj válido, t=0 del segmento = instante de recepción.
// F3: cada vez que cambia el lote/segmento (loteId aparece o desaparece, loteInicioTs, m0 o segmento
// cambian) se incrementa LotParams.generation; el .ino lo usa para reiniciar el estado del modelo (M_prev).
#include <Arduino.h>
#include "drying_model.h"

// ---- Reloj ----
// Epoch actual en ms (UTC); 0 si no hay hora válida (NTP en WiFi; DeviceTimeReq/TIME_HINT en LoRaWAN).
double   clockNowEpochMs();
// Fija la hora a partir de epoch en segundos (DeviceTimeReq / TIME_HINT). Ignora valores anteriores a 2023.
void     clockSetEpochSeconds(uint32_t epochSeconds);
// Tras sincronizar el reloj, vuelve a anclar startMs del lote activo al loteInicioTs recibido.
void     provisioningOnClockSync(LotParams& lot);

// ---- Modo WIFI ----
// Sincroniza la hora por NTP (requiere WiFi ya conectado). Espera hasta timeoutMs. true si quedó sincronizado.
bool provisioningSyncTime(uint32_t timeoutMs);
// Aplica un mensaje de atributos de ThingsBoard a los parámetros vigentes. Maneja:
//  - respuesta a request:  {"shared":{...}}
//  - push de cambios:      {...}            (incluye {"loteId":null} explícito -> fin de lote)
//  - borrado (fin de lote):{"deleted":["loteId",...]}  -> lote inactivo (tipo A)
void applyAttributes(const char* json, unsigned int len,
                     LotParams& lot, float& offsetT, float& offsetRH, uint32_t& publishMs);

// ---- Modo LORAWAN (ESQUEMA §5.4) ----
// Aplica un downlink binario. Comandos: 0x01 SET_LOT, 0x02 CLEAR_LOT, 0x03 SET_NODE, 0x04 TIME_HINT.
void applyDownlink(uint8_t port, const uint8_t* data, size_t len,
                   LotParams& lot, float& offsetT, float& offsetRH, uint32_t& publishMs);

#pragma once
// OLED SSD1306 128x64 del Heltec WiFi LoRa 32 V3 (bus Wire 17/18, Vext=GPIO36).
// Muestra el estado del modelo de secado. Independiente del SHT31 (Wire1).
#include "drying_model.h"

void displayBegin();                                            // enciende Vext + init del OLED
void displayRender(const DryingState& s, const char* netStat);  // pinta un ciclo + estado de red

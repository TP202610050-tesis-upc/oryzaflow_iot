#pragma once
// Transporte WiFi -> MQTT a ThingsBoard (Etapa 3).
// Publica el JSON canónico en v1/devices/me/telemetry.
// Credenciales en secrets.h. Reconexión no bloqueante con backoff.
#include <Arduino.h>

void        netBegin();                 // arranca WiFi + intenta MQTT
void        netLoop();                  // mantiene/reintenta conexión (no bloqueante)
bool        netConnected();             // WiFi + MQTT ok
bool        netPublish(const char* json);  // publica telemetría (false si no conectado)
const char* netStatus();                // texto corto para el OLED

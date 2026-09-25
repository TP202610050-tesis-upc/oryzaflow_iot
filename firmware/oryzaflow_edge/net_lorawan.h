#pragma once
// Transporte LoRaWAN (Etapa 4b/4c): OTAA clase A sobre la pila de Heltec (LoRaWan_APP), sin deep sleep.
// - Uplink: payload binario del codec (codec.h) en LORAWAN_UPLINK_PORT.
// - Downlink: aprovisionamiento binario en LORAWAN_DOWNLINK_PORT (provisioning.h, applyDownlink).
// - Hora: DeviceTimeReq (MAC) al unirse y cada LORAWAN_TIME_REQ_INTERVAL_MS -> handler de tiempo.
// Claves OTAA en secrets.h (LORAWAN_DEV_EUI / LORAWAN_APP_EUI / LORAWAN_APP_KEY, MSB).
// Región: menú de placa LORAWAN_REGION (AU915 = 4); sub-banda por LORAWAN_CHANNEL_MASK (config.h).
#include <Arduino.h>

typedef void (*DownlinkHandler)(uint8_t port, const uint8_t* data, size_t len);
typedef void (*TimeHandler)(uint32_t epochSeconds);

void        lorawanBegin();                               // Mcu.begin + estado INIT (llamar antes que el OLED)
void        lorawanLoop();                                // máquina de estados + timers + IRQ de radio (llamar SIEMPRE, sin delays largos)
bool        lorawanJoined();                              // sesión activa (JoinAccept recibido)
bool        lorawanTxBusy();                              // true durante TX + ventanas RX (evitar trabajo pesado)
bool        lorawanSend(const uint8_t* data, size_t len); // encola un uplink no confirmado (false si no unido/ocupado)
const char* lorawanStatus();                              // texto corto para el OLED
void        lorawanSetDownlinkHandler(DownlinkHandler fn);
void        lorawanSetTimeHandler(TimeHandler fn);
uint32_t    lorawanTxCount();
uint32_t    lorawanRxCount();

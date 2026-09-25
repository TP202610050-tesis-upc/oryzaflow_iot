#include "net_lorawan.h"
#include "config.h"

#if TRANSPORT_MODE == TRANSPORT_LORAWAN
#include "secrets.h"
#include "LoRaWan_APP.h"

// ---- Globales que la pila de Heltec declara extern y exige definir en la aplicación ----
uint8_t devEui[] = LORAWAN_DEV_EUI;
uint8_t appEui[] = LORAWAN_APP_EUI;
uint8_t appKey[] = LORAWAN_APP_KEY;
uint8_t nwkSKey[16] = {0};      // ABP no usado
uint8_t appSKey[16] = {0};
uint32_t devAddr = 0;
uint16_t userChannelsMask[6] = LORAWAN_CHANNEL_MASK;
LoRaMacRegion_t loraWanRegion = ACTIVE_REGION;   // del menú de placa (LORAWAN_REGION)
DeviceClass_t   loraWanClass  = CLASS_A;
uint32_t appTxDutyCycle = 300000;                // no gobierna la cadencia: la lleva el .ino
bool overTheAirActivation = true;
bool loraWanAdr = LORAWAN_ADR;
bool isTxConfirmed = false;
uint8_t appPort = LORAWAN_UPLINK_PORT;
uint8_t confirmedNbTrials = 4;

static DownlinkHandler s_downlink = nullptr;
static TimeHandler     s_time = nullptr;
static bool     s_joinedSeen = false;
static bool     s_timeReqPending = true;         // el primer uplink tras el join pide la hora
static uint32_t s_lastTimeReqMs = 0;
static uint32_t s_lastTxMs = 0;
static uint32_t s_txCount = 0, s_rxCount = 0;
static char     s_status[20] = "lora init";

static void setStatus(const char* s) { strncpy(s_status, s, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

void lorawanSetDownlinkHandler(DownlinkHandler fn) { s_downlink = fn; }
void lorawanSetTimeHandler(TimeHandler fn) { s_time = fn; }
const char* lorawanStatus() { return s_status; }
uint32_t lorawanTxCount() { return s_txCount; }
uint32_t lorawanRxCount() { return s_rxCount; }

void lorawanBegin() {
#ifdef HELTEC_LICENSE
  // La pila LoRaWAN precompilada de Heltec exige una licencia gratuita por chip (4 x uint32). Se obtiene en
  // https://resource.heltec.cn/search con el ChipID que imprime Mcu.begin(); se guarda en secrets.h (fuera de git).
  static uint32_t license[4] = HELTEC_LICENSE;
  Mcu.setlicense(license, HELTEC_BOARD);
#else
  Serial.println("[lora] AVISO: sin HELTEC_LICENSE en secrets.h; Mcu.begin() se bloqueara pidiendo la licencia");
#endif
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);   // SPI + SX1262 + temporizadores de la pila
  deviceState = DEVICE_STATE_INIT;
  setStatus("lora init");
}

bool lorawanJoined() {
  MibRequestConfirm_t mib;
  mib.Type = MIB_NETWORK_JOINED;
  if (LoRaMacMibGetRequestConfirm(&mib) != LORAMAC_STATUS_OK) return false;
  return mib.Param.IsNetworkJoined;
}

// TX + RX1 (5 s en TTS) + RX2 (6 s) + margen: en esta ventana el loop debe limitarse a atender la pila.
bool lorawanTxBusy() {
  return s_lastTxMs != 0 && (millis() - s_lastTxMs) < LORAWAN_TX_BUSY_MS;
}

// Máquina de estados de Heltec SIN LoRaWAN.sleep(): esa función entra en deep sleep (esp_deep_sleep_start) y
// borraría la RAM del modelo (ModelState) y el OLED. En su lugar se atienden los temporizadores software de la
// pila (Mcu.timerhandler) y las interrupciones de la radio (Radio.IrqProcess) en cada vuelta del loop.
void lorawanLoop() {
  switch (deviceState) {
    case DEVICE_STATE_INIT:
      LoRaWAN.init(loraWanClass, loraWanRegion);   // deja deviceState = JOIN
      LoRaWAN.setDefaultDR(LORAWAN_DEFAULT_DR);
      setStatus("lora join");
      break;
    case DEVICE_STATE_JOIN:
      LoRaWAN.join();                              // SLEEP (esperando JoinAccept) o CYCLE (no se pudo encolar)
      break;
    case DEVICE_STATE_SEND:
      // La pila pone SEND al unirse o al vencer su temporizador interno; aquí no se envía nada:
      // la cadencia la gobierna el .ino con lorawanSend().
      if (!s_joinedSeen) { s_joinedSeen = true; s_timeReqPending = true; setStatus("lora ok"); Serial.println("[lora] unido (OTAA)"); }
      deviceState = DEVICE_STATE_SLEEP;
      break;
    case DEVICE_STATE_CYCLE:
      LoRaWAN.cycle(LORAWAN_REJOIN_DELAY_MS);      // reintento de join por temporizador de la pila
      deviceState = DEVICE_STATE_SLEEP;
      break;
    case DEVICE_STATE_SLEEP:
    default:
      Mcu.timerhandler();
      Radio.IrqProcess();
      break;
  }
}

bool lorawanSend(const uint8_t* data, size_t len) {
  if (!lorawanJoined()) { setStatus("lora join"); return false; }
  if (lorawanTxBusy()) return false;
  if (len == 0 || len > LORAWAN_APP_DATA_MAX_SIZE) return false;

  // Hora de red (DeviceTimeReq, LoRaWAN >= 1.0.3 en el servidor): en el primer uplink y luego periódicamente.
  if (s_timeReqPending || (millis() - s_lastTimeReqMs) > LORAWAN_TIME_REQ_INTERVAL_MS) {
    MlmeReq_t mlme;
    mlme.Type = MLME_DEVICE_TIME;
    if (LoRaMacMlmeRequest(&mlme) == LORAMAC_STATUS_OK) {
      s_timeReqPending = false;
      s_lastTimeReqMs = millis();
      Serial.println("[lora] DeviceTimeReq anexado al uplink");
    }
  }

  lwan_dev_params_update();                      // aplica la máscara de canales (sub-banda) antes de cada TX
  LoRaMacTxInfo_t txInfo;
  if (LoRaMacQueryTxPossible(len, &txInfo) != LORAMAC_STATUS_OK) {
    Serial.printf("[lora] payload de %u B no cabe en el DR actual (max %u); se envía vacío para bajar/ajustar DR\n",
                  (unsigned)len, (unsigned)txInfo.MaxPossiblePayload);
    McpsReq_t empty;
    empty.Type = MCPS_UNCONFIRMED;
    empty.Req.Unconfirmed.fBuffer = NULL;
    empty.Req.Unconfirmed.fBufferSize = 0;
    empty.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DR;
    LoRaMacMcpsRequest(&empty);
    s_lastTxMs = millis();
    return false;
  }
  memcpy(appData, data, len);
  appDataSize = len;
  McpsReq_t req;
  req.Type = MCPS_UNCONFIRMED;
  req.Req.Unconfirmed.fPort = LORAWAN_UPLINK_PORT;
  req.Req.Unconfirmed.fBuffer = appData;
  req.Req.Unconfirmed.fBufferSize = appDataSize;
  req.Req.Unconfirmed.Datarate = LORAWAN_DEFAULT_DR;   // con ADR la red lo ajusta
  LoRaMacStatus_t st = LoRaMacMcpsRequest(&req);
  if (st != LORAMAC_STATUS_OK) {
    Serial.printf("[lora] uplink rechazado por la pila: status=%d\n", (int)st);
    setStatus("lora tx ERR");
    return false;
  }
  s_lastTxMs = millis();
  s_txCount++;
  deviceState = DEVICE_STATE_SLEEP;
  setStatus("lora ok");
  return true;
}

// ---- Callbacks que la pila invoca (sustituyen a las versiones weak de LoRaWan_APP.cpp) ----
void downLinkDataHandle(McpsIndication_t* ind) {
  s_rxCount++;
  Serial.printf("[lora] downlink puerto %u, %u B, rssi %d, snr %d: ", (unsigned)ind->Port, (unsigned)ind->BufferSize,
                ind->Rssi, (int)ind->Snr);
  for (uint8_t i = 0; i < ind->BufferSize; i++) Serial.printf("%02X", ind->Buffer[i]);
  Serial.println();
  if (s_downlink) s_downlink(ind->Port, ind->Buffer, ind->BufferSize);
}

void dev_time_updated() {
  TimerSysTime_t t = TimerGetSysTime();
  Serial.printf("[lora] hora de red recibida: epoch=%lu\n", (unsigned long)t.Seconds);
  if (s_time) s_time((uint32_t)t.Seconds);
}

#endif  // TRANSPORT_MODE == TRANSPORT_LORAWAN

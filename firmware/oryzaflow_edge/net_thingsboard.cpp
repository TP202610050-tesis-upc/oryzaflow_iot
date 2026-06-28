#include "net_thingsboard.h"
#include "secrets.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static char         statusBuf[20] = "init";
static uint32_t     lastMqttTry = 0;
static const uint32_t MQTT_RETRY_MS = 5000;
static AttrHandler  attrHandler = nullptr;

static void setStatus(const char* s) { strncpy(statusBuf, s, sizeof(statusBuf) - 1); }

void netSetAttributeHandler(AttrHandler fn) { attrHandler = fn; }

// Atributos de ThingsBoard: tópicos v1/devices/me/attributes (push) y .../response/+ (request).
static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  if (attrHandler && strncmp(topic, "v1/devices/me/attributes", 24) == 0)
    attrHandler((const char*)payload, len);
}

static void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] conectando a \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] OK  ip=%s  rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    setStatus("wifi ok");
  } else {
    Serial.println("[wifi] FALLO (revisar SSID/clave/2.4GHz)");
    setStatus("wifi FAIL");
  }
}

static bool mqttConnect() {
  if (mqtt.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return false;  // backoff
  lastMqttTry = millis();

  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setBufferSize(512);
  Serial.printf("[mqtt] conectando a %s:%d ...\n", TB_HOST, TB_PORT);
  // ThingsBoard: clientId libre, username = access token, password vacío.
  if (mqtt.connect("oryzaflow-edge-01", TB_TOKEN, "")) {
    Serial.println("[mqtt] CONECTADO a ThingsBoard");
    setStatus("TB ok");
    // Etapa 4a: escuchar cambios de shared attributes y pedir el estado actual del lote.
    mqtt.subscribe("v1/devices/me/attributes");
    mqtt.subscribe("v1/devices/me/attributes/response/+");
    mqtt.publish("v1/devices/me/attributes/request/1", "{\"sharedKeys\":\"" TB_SHARED_KEYS "\"}");
    Serial.println("[mqtt] suscrito a atributos + pedido de aprovisionamiento");
    return true;
  }
  Serial.printf("[mqtt] fallo rc=%d (5=token malo, 2=rechazado)\n", mqtt.state());
  setStatus("TB FAIL");
  return false;
}

void netBegin() {
  mqtt.setCallback(onMqttMessage);
  wifiConnect();
  // El primer mqttConnect ocurre en netLoop, DESPUÉS de que setup sincronice NTP,
  // para que el aprovisionamiento (loteInicioTs) se resuelva con hora real.
}

void netLoop() {
  if (WiFi.status() != WL_CONNECTED) { wifiConnect(); return; }
  if (!mqtt.connected())            { mqttConnect();      }
  mqtt.loop();
}

bool netConnected() {
  return WiFi.status() == WL_CONNECTED && mqtt.connected();
}

bool netPublish(const char* json) {
  if (!mqtt.connected()) return false;
  return mqtt.publish("v1/devices/me/telemetry", json);
}

const char* netStatus() { return statusBuf; }

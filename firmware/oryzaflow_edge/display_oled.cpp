#include "display_oled.h"
#include "HT_SSD1306Wire.h"   // librería Heltec_ESP32_Dev-Boards
#include "config.h"

// Pines OLED y Vext los define el core para la placa V3 (SDA_OLED=17, SCL_OLED=18,
// RST_OLED=21, Vext=36).
static SSD1306Wire oled(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

void displayBegin() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);   // Vext ON (LOW alimenta el OLED en el V3)
  delay(50);

  oled.init();
  oled.setContrast(255);
  oled.clear();
  oled.setFont(ArialMT_Plain_16);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "OryzaFlow");
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 20, "edge v" FW_VERSION);
  oled.drawString(0, 34, "iniciando...");
  oled.display();
}

void displayRender(const DryingState& s, const char* netStat) {
  char line[40];
  oled.clear();
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setFont(ArialMT_Plain_10);

  snprintf(line, sizeof(line), "T:%.1fC  HR:%.0f%%", s.tC, s.rhPct);
  oled.drawString(0, 0, line);

  snprintf(line, sizeof(line), "lluvia: %s", s.rain ? "SI" : "no");
  oled.drawString(0, 13, line);

  if (s.hasB) {
    snprintf(line, sizeof(line), "M:%.1f  me:%.1f", s.m, s.me);
    oled.drawString(0, 28, line);
    if (s.te > 0)
      snprintf(line, sizeof(line), "te:%.1fh  DR:%.2f", s.te, s.hasDr ? s.dr : 0.0f);
    else
      snprintf(line, sizeof(line), "meta inviable (me>=Mt)");
    oled.drawString(0, 41, line);
  } else {
    snprintf(line, sizeof(line), "me:%.1f", s.me);
    oled.drawString(0, 28, line);
    oled.drawString(0, 41, "sin lote activo");
  }

  snprintf(line, sizeof(line), "TB: %s", netStat);
  oled.drawString(0, 54, line);
  oled.display();
}

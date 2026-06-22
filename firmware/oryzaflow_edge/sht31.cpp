#include "sht31.h"

bool Sht31::begin() {
  // Soft reset: 0x30A2
  _bus.beginTransmission(_addr);
  _bus.write(0x30);
  _bus.write(0xA2);
  bool ok = (_bus.endTransmission() == 0);
  delay(2);
  return ok;
}

// CRC-8, polinomio 0x31, init 0xFF (datasheet SHT3x)
uint8_t Sht31::crc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

Sht31Reading Sht31::read() {
  Sht31Reading r{false, NAN, NAN};

  // Single-shot, alta repetibilidad, sin clock stretching: 0x2400
  _bus.beginTransmission(_addr);
  _bus.write(0x24);
  _bus.write(0x00);
  if (_bus.endTransmission() != 0) return r;

  delay(20);  // t_meas alta repetibilidad ~15 ms

  if (_bus.requestFrom((int)_addr, (int)6) != 6) return r;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = _bus.read();

  // Verificación CRC de ambos words
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return r;

  uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];
  r.tempC  = -45.0f + 175.0f * (float)rawT / 65535.0f;
  r.humPct = 100.0f * (float)rawH / 65535.0f;
  r.ok = true;
  return r;
}

#pragma once
// Driver mínimo SHT31 por I2C (single-shot, alta repetibilidad, sin clock-stretch).
// Sin dependencias externas: solo Wire. El bus debe estar ya inicializado (begin del bus)
// antes de llamar a begin()/read().
#include <Arduino.h>
#include <Wire.h>

struct Sht31Reading {
  bool  ok;       // true si la lectura (y su CRC) son válidas
  float tempC;    // °C
  float humPct;   // % HR
};

class Sht31 {
public:
  Sht31(TwoWire& bus, uint8_t addr) : _bus(bus), _addr(addr) {}

  // Soft-reset del sensor. Devuelve true si el sensor respondió (ACK).
  bool begin();

  // Medición puntual de alta repetibilidad. ok=false si NACK o CRC inválido.
  Sht31Reading read();

private:
  TwoWire& _bus;
  uint8_t  _addr;
  static uint8_t crc8(const uint8_t* data, int len);
};

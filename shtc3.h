// ============================================================
//  shtc3.h - minimal SHTC3 temperature/humidity driver
//
//  Hand-rolled on top of Wire so we do not pull in SensorLib:
//  the app partition is already ~85% full and this needs ~50
//  lines instead of a whole sensor framework.
//
//  The sensor sits on the board's shared I2C bus at 0x70
//  (SDA=13, SCL=14).  It returns to sleep after every measurement
//  and must be woken before each command.
// ============================================================
#ifndef SHTC3_H
#define SHTC3_H

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#define SHTC3_I2C_ADDR 0x70

// CRC-8, polynomial 0x31, init 0xFF (SHTC3 datasheet 4.4).
inline uint8_t shtc3Crc(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// Wake the sensor and send a command. Returns true on ACK.
inline bool shtc3Command(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_I2C_ADDR);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

// Is anything answering at 0x70?
inline bool shtc3Probe() {
  bool ok = shtc3Command(0x3517);   // wake-up
  if (ok) shtc3Command(0xB098);     // sleep
  return ok;
}

// Read temperature and humidity. Values are returned in
// hundredths (2853 = 28.53 degC, 6108 = 61.08 %RH).
inline bool shtc3Read(int16_t& tempCenti, int16_t& rhCenti) {
  if (!shtc3Command(0x3517)) return false;      // wake-up
  delayMicroseconds(300);

  // 0x7866 = measure, temperature first, no clock stretching.
  if (!shtc3Command(0x7866)) return false;
  delay(13);                                     // max conversion time

  if (Wire.requestFrom((uint8_t)SHTC3_I2C_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (uint8_t i = 0; i < 6; i++) b[i] = (uint8_t)Wire.read();

  if (shtc3Crc(b, 2) != b[2] || shtc3Crc(b + 3, 2) != b[5]) return false;

  uint16_t tRaw = ((uint16_t)b[0] << 8) | b[1];
  uint16_t hRaw = ((uint16_t)b[3] << 8) | b[4];

  float tC = -45.0f + 175.0f * (float)tRaw / 65535.0f;
  float rh = 100.0f * (float)hRaw / 65535.0f;

  tempCenti = (int16_t)lroundf(tC * 100.0f);
  rhCenti   = (int16_t)lroundf(rh * 100.0f);

  shtc3Command(0xB098);                          // back to sleep
  return true;
}

#endif // SHTC3_H

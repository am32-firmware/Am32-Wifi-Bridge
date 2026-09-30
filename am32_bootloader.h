#ifndef AM32_BOOTLOADER_H
#define AM32_BOOTLOADER_H

#include <Arduino.h>

#include "am32_single_wire.h"

// Port of the Offline-Configurator "USB / Arduino Connect" (direct) path:
// BF_ROOTLOADER packet framing plus Widget::connectMotor(), readInitData()
// and sendDirect(), talking straight to the AM32 bootloader.
class Am32Bootloader {
 public:
  explicit Am32Bootloader(SingleWireSerial &wire) : wire(wire) {}

  // Sends the BLHeli boot-init sequence and parses the 9 byte device info.
  bool connect();

  // Reads the 32 byte file name area plus the 48 byte settings block that
  // follows it (the same 80 byte read the Qt tool does).
  bool readSettings(uint8_t name[32], uint8_t eeprom[48]);

  // Set address, set buffer size, send buffer, write flash.
  bool sendDirect(const uint8_t *buf, uint16_t len, uint16_t address);

  // Writes the 48 byte settings block at eepromAddress (0x7C00 on 32k parts),
  // like Widget::on_writeEEPROM_clicked().
  bool writeEeprom(const uint8_t *buf, uint16_t len);

  // Writes one chunk of a firmware image; offset is in bytes from the start
  // of the application area.
  bool writeFirmwareChunk(uint32_t offset, const uint8_t *buf, uint16_t len);

  // CMD_RUN: leave the bootloader and start the ESC firmware.
  void runApplication();

  // Size in bytes of the area between the firmware start and the settings.
  uint32_t firmwareAreaSize() const;

  static uint16_t crc16(const uint8_t *buf, size_t len);

  bool connected = false;
  uint8_t deviceInfo[9] = {0};
  uint16_t eepromAddress = 0;   // as sent in CMD_SET_ADDRESS (already shifted)
  uint32_t firmwareStart = 0;   // byte offset of the application from flash start
  bool addressShift = false;    // 128k parts: addresses are sent >> 2
  uint8_t protocolVersion = 0;
  String lastError;

 private:
  bool setAddress(uint16_t address);
  bool readFlash(uint16_t len, uint8_t *out);
  void sendPacket(const uint8_t *body, size_t len);
  bool sendPacketExpectAck(const uint8_t *body, size_t len, uint32_t timeoutUs);

  uint8_t lastReply = 0;  // last byte of the most recent reply, 0 if none

  SingleWireSerial &wire;
};

#endif  // AM32_BOOTLOADER_H

#include "am32_bootloader.h"

// Bootloader commands (AM32-bootloader/bootloader/main.c)
static const uint8_t CMD_RUN = 0x00;
static const uint8_t CMD_PROG_FLASH = 0x01;
static const uint8_t CMD_READ_FLASH_SIL = 0x03;
static const uint8_t CMD_SET_BUFFER = 0xFE;
static const uint8_t CMD_SET_ADDRESS = 0xFF;

static const uint8_t ACK_OK = 0x30;

// Magic CMD_SET_ADDRESS value for EEPROM_START_ADD - 32 (bootloader protocol 2+)
static const uint16_t ADDRESS_MAGIC_FILE_NAME = 0x21;

// Gap between packets. The bootloader needs 5 bit times of silence to end a
// packet and then has to get back to receiving; the Qt tool's sendDirect()
// leaves at least 20-30ms between packets, so match that.
static const uint32_t kInterPacketDelayMs = 20;

// Reply timeouts, each at least as long as the Qt tool's
// waitForBytesWritten() + waitForReadyRead() for the same step.
static const uint32_t kSetAddressAckUs = 50000;     // Qt: 10 + 20ms
static const uint32_t kBufferAckUs = 150000;        // Qt: 20 + 75ms
static const uint32_t kFlashWriteAckUs = 500000;    // Qt: 10 + 30ms (+ erase margin)
static const uint32_t kReadReplyUs = 50000;         // Qt: 200ms for the whole read

uint16_t Am32Bootloader::crc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t xb = buf[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (((xb & 0x01) ^ (crc & 0x0001)) != 0) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
      xb >>= 1;
    }
  }
  return crc;
}

static size_t frame(const uint8_t *body, size_t len, uint8_t *out) {
  memcpy(out, body, len);
  const uint16_t crc = Am32Bootloader::crc16(body, len);
  out[len] = crc & 0xFF;
  out[len + 1] = crc >> 8;
  return len + 2;
}

// For CMD_SET_BUFFER, which the bootloader does not answer.
void Am32Bootloader::sendPacket(const uint8_t *body, size_t len) {
  uint8_t out[260];
  wire.write(out, frame(body, len, out));
}

// Sends a packet and listens for the reply without letting another task run
// in between. Returns true when the reply ends with ACK_OK.
bool Am32Bootloader::sendPacketExpectAck(const uint8_t *body, size_t len, uint32_t timeoutUs) {
  uint8_t out[260];
  uint8_t rx[4];
  const size_t n = wire.transfer(out, frame(body, len, out), rx, sizeof(rx), timeoutUs);
  lastReply = n > 0 ? rx[n - 1] : 0;
  return n > 0 && rx[n - 1] == ACK_OK;
}

static String replyText(uint8_t reply) {
  switch (reply) {
    case 0:
      return "no reply";
    case 0xC1:
      return "BAD_ACK 0xC1";
    case 0xC2:
      return "BAD_CRC 0xC2";
    default:
      return "reply 0x" + String(reply, HEX);
  }
}

bool Am32Bootloader::connect() {
  connected = false;

  // Same boot-init sequence Widget::connectMotor() sends in direct mode.
  static const uint8_t init[21] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0x0D, 'B', 'L', 'H', 'e', 'l', 'i', 0xF4, 0x7D};
  uint8_t rx[16];
  const size_t n = wire.transfer(init, sizeof(init), rx, sizeof(rx), 50000);
  // Unlike a USB-serial adapter wired to one line we never hear our own
  // echo, so the device info is at the start of the buffer.
  if (n < 9 || rx[8] != ACK_OK) {
    lastError = n == 0 ? "No response from ESC bootloader" : "Bad device info from ESC";
    return false;
  }
  memcpy(deviceInfo, rx, sizeof(deviceInfo));
  protocolVersion = deviceInfo[7];

  // Widget::readInitData()
  switch (deviceInfo[4]) {
    case 0x2B:  // G071 etc, 128k, 2k pages
      addressShift = true;
      eepromAddress = 0x7E00;  // 0x1F800 >> 2
      firmwareStart = 4096;
      break;
    case 0x1F:  // F0, 32k, 1k pages
      addressShift = false;
      eepromAddress = 0x7C00;
      firmwareStart = 4096;
      break;
    case 0x35:  // 64k, 2k pages
      addressShift = false;
      eepromAddress = 0xF800;
      firmwareStart = 4096;
      break;
    case 0x15:  // NXP 64k, 8k pages
      addressShift = false;
      eepromAddress = 0xE000;
      firmwareStart = 16384;
      break;
    default:
      lastError = "Unknown flash size code 0x" + String(deviceInfo[4], HEX);
      return false;
  }

  connected = true;
  return true;
}

bool Am32Bootloader::setAddress(uint16_t address) {
  const uint8_t cmd[4] = {CMD_SET_ADDRESS, 0x00, (uint8_t)(address >> 8), (uint8_t)(address & 0xFF)};
  return sendPacketExpectAck(cmd, sizeof(cmd), kSetAddressAckUs);
}

bool Am32Bootloader::readFlash(uint16_t len, uint8_t *out) {
  const uint8_t cmd[2] = {CMD_READ_FLASH_SIL, (uint8_t)(len == 256 ? 0 : len)};
  uint8_t tx[4];

  // data, crc low, crc high, ack
  uint8_t rx[259];
  const size_t n = wire.transfer(tx, frame(cmd, sizeof(cmd), tx), rx, len + 3, kReadReplyUs);
  if (n != (size_t)len + 3 || rx[len + 2] != ACK_OK) {
    lastError = "Read failed (" + String(n) + " of " + String(len + 3) + " bytes)";
    return false;
  }
  const uint16_t crc = rx[len] | (rx[len + 1] << 8);
  if (crc != crc16(rx, len)) {
    lastError = "Read CRC error";
    return false;
  }
  memcpy(out, rx, len);
  return true;
}

bool Am32Bootloader::readSettings(uint8_t name[32], uint8_t eeprom[48]) {
  if (!connected) {
    lastError = "Not connected";
    return false;
  }
  // The Qt tool uses eeprom_address - 32, which is only right for unshifted
  // addresses. Bootloader protocol 2+ has a magic address for exactly this
  // spot, so prefer it and fall back to a correctly shifted address.
  uint16_t address;
  if (protocolVersion >= 2) {
    address = ADDRESS_MAGIC_FILE_NAME;
  } else {
    address = eepromAddress - (addressShift ? 32 / 4 : 32);
  }
  if (!setAddress(address)) {
    lastError = "Set address 0x" + String(address, HEX) + ": " + replyText(lastReply);
    return false;
  }
  delay(kInterPacketDelayMs);

  uint8_t buf[80];
  if (!readFlash(sizeof(buf), buf)) {
    return false;
  }
  memcpy(name, buf, 32);
  memcpy(eeprom, buf + 32, 48);
  return true;
}

bool Am32Bootloader::sendDirect(const uint8_t *buf, uint16_t len, uint16_t address) {
  if (!connected) {
    lastError = "Not connected";
    return false;
  }
  if (len == 0 || len > 256) {
    lastError = "Bad buffer size";
    return false;
  }

  if (!setAddress(address)) {
    lastError = "Set address 0x" + String(address, HEX) + ": " + replyText(lastReply);
    return false;
  }
  delay(kInterPacketDelayMs);

  // The bootloader does not ack CMD_SET_BUFFER.
  const uint8_t setBuffer[4] = {CMD_SET_BUFFER, 0x00, (uint8_t)(len == 256 ? 0x01 : 0x00),
                                (uint8_t)(len == 256 ? 0x00 : len)};
  sendPacket(setBuffer, sizeof(setBuffer));
  delay(kInterPacketDelayMs);

  if (!sendPacketExpectAck(buf, len, kBufferAckUs)) {
    lastError = "Send " + String(len) + " byte buffer: " + replyText(lastReply);
    return false;
  }
  delay(kInterPacketDelayMs);

  const uint8_t prog[2] = {CMD_PROG_FLASH, 0x01};
  // Allows for a page erase before the write on the slowest parts.
  if (!sendPacketExpectAck(prog, sizeof(prog), kFlashWriteAckUs)) {
    lastError = "Flash write at 0x" + String(address, HEX) + ": " + replyText(lastReply);
    return false;
  }
  return true;
}

bool Am32Bootloader::writeEeprom(const uint8_t *buf, uint16_t len) {
  if (len != 48) {
    lastError = "Settings block must be 48 bytes, got " + String(len);
    return false;
  }
  Serial.printf("write settings: %u bytes at 0x%04x\n", len, eepromAddress);
  const bool ok = sendDirect(buf, len, eepromAddress);
  if (!ok) {
    Serial.printf("write settings failed: %s\n", lastError.c_str());
  }
  return ok;
}

uint32_t Am32Bootloader::firmwareAreaSize() const {
  const uint32_t eepromByte = (uint32_t)eepromAddress << (addressShift ? 2 : 0);
  return eepromByte > firmwareStart ? eepromByte - firmwareStart : 0;
}

bool Am32Bootloader::writeFirmwareChunk(uint32_t offset, const uint8_t *buf, uint16_t len) {
  if (!connected) {
    lastError = "Not connected";
    return false;
  }
  if (offset + len > firmwareAreaSize()) {
    lastError = "Chunk would overwrite the settings area";
    return false;
  }
  uint32_t address = firmwareStart + offset;
  if (addressShift) {
    address >>= 2;
  }
  return sendDirect(buf, len, (uint16_t)address);
}

void Am32Bootloader::runApplication() {
  // Widget::resetESC() in direct mode: four zero bytes.
  static const uint8_t reset[4] = {CMD_RUN, 0, 0, 0};
  // decodeInput() falls through after CMD_RUN and still sends a 0xC1 before
  // jumping, so listen for it rather than driving the line against the ESC.
  uint8_t rx[4];
  wire.transfer(reset, sizeof(reset), rx, sizeof(rx), 20000);
  connected = false;
}

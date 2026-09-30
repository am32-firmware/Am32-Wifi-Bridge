#ifndef AM32_SINGLE_WIRE_H
#define AM32_SINGLE_WIRE_H

#include <Arduino.h>

// Bit-banged, half-duplex UART on a single GPIO (8N1, idle high, not
// inverted) matching the AM32 bootloader's own bit-banged serial in
// AM32-bootloader/bootloader/main.c (serialreadChar / serialwriteChar).
//
// The pin is switched between push-pull output (while we transmit, and while
// idle so the ESC sees a high signal line at power-up and stays in its
// bootloader) and input with pull-up (while we wait for the ESC to answer).
// No diode / resistor / TX-RX bridge is required.
class SingleWireSerial {
 public:
  void begin(uint8_t pin, uint32_t baud);

  // Drive the line high (UART idle). Called between transactions.
  void driveIdleHigh();

  // Stop driving the line; the pull-ups on both ends keep it high.
  void release();

  // Transmit bytes back to back, then release the line so the ESC can answer.
  void write(const uint8_t *data, size_t len);

  // Receive up to maxLen bytes. Waits up to firstByteTimeoutUs for the first
  // start bit, then ends the frame once the line has been idle for
  // interByteTimeoutUs. Returns the number of good bytes received.
  size_t read(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
              uint32_t interByteTimeoutUs = 1000);

  // write() immediately followed by read(), with no chance for another task
  // to run in between and miss the start of the reply.
  size_t transfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxMax,
                  uint32_t firstByteTimeoutUs, uint32_t interByteTimeoutUs = 1000);

  bool lastFramingError() const { return framingError; }

 private:
  void writeBytes(const uint8_t *data, size_t len);
  size_t readBytes(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
                   uint32_t interByteTimeoutUs);
  size_t readFrameLocked(uint8_t *buf, size_t maxLen, uint32_t edge,
                         uint32_t interByteCycles);

  uint32_t mask = 0;
  uint32_t cpuMhz = 160;
  uint32_t edgeOffset[11];    // cycles from start-bit edge to each bit edge
  uint32_t sampleOffset[10];  // cycles from start-bit edge to each bit centre
  bool framingError = false;
};

#endif  // AM32_SINGLE_WIRE_H

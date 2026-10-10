#ifndef AM32_SINGLE_WIRE_H
#define AM32_SINGLE_WIRE_H

#include <Arduino.h>

#include "driver/uart.h"

// Half-duplex UART on a single GPIO (8N1, idle high, not inverted) talking to
// the AM32 bootloader's bit-banged serial (AM32-bootloader/bootloader/main.c).
//
// Uses hardware UART1 routed through the GPIO matrix, so WiFi interrupts and
// task switches can no longer disturb the bit timing. During a transaction
// the pin is open-drain with both UART TX and RX attached to it: we only ever
// pull the line low, the pull-ups hold it high, and the ESC can answer at any
// moment without the two sides driving against each other. Between
// transactions the pin is a plain push-pull GPIO driven high, so the ESC sees
// a high signal line at power-up and stays in its bootloader, and the signal
// passthrough can take the pin over. No diode / resistor / TX-RX bridge is
// required.
class SingleWireSerial {
 public:
  void begin(uint8_t pin, uint32_t baud);

  // Detach the UART and drive the line high as a plain GPIO (UART idle).
  // The pin is left in this state after every transaction.
  void driveIdleHigh();

  // Transmit bytes back to back. Our own echo is read back and checked.
  void write(const uint8_t *data, size_t len);

  // Receive up to maxLen bytes. Waits up to firstByteTimeoutUs for the first
  // byte, then ends the frame once no byte has come for interByteTimeoutUs.
  // Returns the number of good bytes received.
  size_t read(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
              uint32_t interByteTimeoutUs = 1000);

  // write() followed by read(). The UART keeps listening while it sends, so
  // the start of the reply can never be missed.
  size_t transfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxMax,
                  uint32_t firstByteTimeoutUs, uint32_t interByteTimeoutUs = 1000);

  // A received byte had a bad stop bit / break, or the RX FIFO overflowed.
  bool lastFramingError() const { return framingError; }
  // What we heard of our own transmission differed from what we sent
  // (collision with the ESC, or a weak pull-up).
  bool lastEchoError() const { return echoError; }

 private:
  void attach();
  void writeBytes(const uint8_t *data, size_t len);
  size_t readBytes(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
                   uint32_t interByteTimeoutUs);
  void checkEvents();

  uint8_t pin = 0;
  uint32_t baud = 19200;
  QueueHandle_t events = nullptr;
  bool framingError = false;
  bool echoError = false;
};

#endif  // AM32_SINGLE_WIRE_H

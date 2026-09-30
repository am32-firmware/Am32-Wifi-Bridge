#ifndef SIGNAL_PASSTHROUGH_H
#define SIGNAL_PASSTHROUGH_H

#include <Arduino.h>

// Copies the level of an input pin to the ESC signal pin in a tight register
// polling loop, so a receiver / flight controller signal (PWM, Oneshot,
// Multishot, DShot) reaches the ESC while nobody is using the configurator.
//
// Every rising edge is checked: if the loop could have been held up by an
// interrupt or another task just before it saw the edge, the edge time is
// unknown and the output pulse would come out short. That whole pulse /
// DShot frame is then dropped (output held low) instead of forwarded.
class SignalPassthrough {
 public:
  void begin(uint8_t inputPin, uint8_t outputPin);

  // Mirrors input -> output for roughly durationMs, then returns so the
  // caller can check whether WiFi is in use. Always returns between pulses
  // with the output low.
  void run(uint32_t durationMs);

  uint32_t forwardedPulses() const { return forwarded; }
  uint32_t droppedPulses() const { return dropped; }

 private:
  bool mirrorBurst();
  void calibrate();

  uint32_t inMask = 0;
  uint32_t outMask = 0;
  uint32_t cpuMhz = 160;
  uint32_t idleCycles = 0;
  uint32_t maxBurstCycles = 0;
  uint32_t maxGapCycles = 0;  // longest normal time between two samples
  uint32_t forwarded = 0;
  uint32_t dropped = 0;
};

#endif  // SIGNAL_PASSTHROUGH_H

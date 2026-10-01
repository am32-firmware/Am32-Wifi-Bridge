#ifndef SIGNAL_PASSTHROUGH_H
#define SIGNAL_PASSTHROUGH_H

#include <Arduino.h>

// Copies the level of an input pin to the ESC signal pin in a tight register
// polling loop, so a receiver / flight controller signal (PWM, Oneshot,
// Multishot, DShot) reaches the ESC while nobody is using the configurator.
//
// Interrupts are the enemy of an exact copy, so once the signal's period is
// known they are only allowed in the quiet gap after each pulse. Around every
// expected rising edge interrupts are held off, and pending ones run after
// the pulse. This keeps periodic interrupts (RTOS tick, WiFi timers) from
// beating against the signal frequency.
//
// Any rising edge that may still have been seen late (unknown period,
// irregular signal) is detected and that whole pulse / DShot frame is
// dropped (output held low) instead of forwarded short.
class SignalPassthrough {
 public:
  void begin(uint8_t inputPin, uint8_t outputPin);

  // Mirrors input -> output for roughly durationMs, then returns so the
  // caller can check whether WiFi is in use. Returns in the gap between
  // pulses, with the output low and enough time left before the next
  // expected pulse for the caller's short check.
  void run(uint32_t durationMs);

  uint32_t forwardedPulses() const { return forwarded; }
  uint32_t droppedPulses() const { return dropped; }
  // Learned signal period in microseconds, 0 while not locked on.
  uint32_t periodUs() const { return stable >= kStableNeeded ? period / cpuMhz : 0; }

 private:
  static const uint8_t kStableNeeded = 3;

  bool mirrorBurst();
  void notePulse(uint32_t rise);
  void calibrate();

  uint32_t inMask = 0;
  uint32_t outMask = 0;
  uint32_t cpuMhz = 160;
  uint32_t idleCycles = 0;
  uint32_t maxBurstCycles = 0;
  uint32_t maxGapCycles = 0;  // longest normal time between two samples

  // Signal period tracking (all in CPU cycles).
  bool haveLastRise = false;
  uint32_t lastRise = 0;
  uint32_t period = 0;
  uint32_t guard = 0;          // interrupts held off this long either side of an expected edge
  uint32_t nextExpected = 0;
  uint8_t stable = 0;          // consecutive pulses matching the period
  uint8_t misses = 0;          // consecutive expected pulses that did not come

  uint32_t forwarded = 0;
  uint32_t dropped = 0;
};

#endif  // SIGNAL_PASSTHROUGH_H

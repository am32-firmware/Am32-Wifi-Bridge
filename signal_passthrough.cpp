#include "signal_passthrough.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "riscv/csr.h"
#include "soc/gpio_reg.h"

// A burst (one pulse, or one whole DShot frame) ends once the input has been
// low for kIdleUs, longer than any low time inside a DShot150 frame (~4.2us).
// A rising edge only starts a new burst after this much verified low time.
static const uint32_t kIdleUs = 10;

// Upper bound on one interrupts-off burst (e.g. input stuck high). Longer
// than any valid servo pulse, well under the 300ms interrupt watchdog.
static const uint32_t kMaxBurstUs = 5000;

// Extra slack allowed on top of the calibrated sample-to-sample time before
// a rising edge is treated as possibly late. Any interrupt handler takes
// longer than this.
static const uint32_t kGapSlackNs = 250;

// Interrupts are held off from (expected edge - guard) until the pulse
// arrives, or until (expected edge + guard) if it does not. The guard covers
// the source's own period jitter: period / 16, clamped to this range.
static const uint32_t kMinGuardUs = 30;
static const uint32_t kMaxGuardUs = 2000;

// Periods the scheduler locks on to (DShot at 8kHz ... 50Hz servo and below).
static const uint32_t kMinPeriodUs = 60;
static const uint32_t kMaxPeriodUs = 60000;

// Expected pulses that may go missing before the schedule is dropped.
static const uint8_t kMaxMisses = 4;

// run() only returns to the caller with at least this much time before the
// interrupts-off window of the next expected pulse, so the WiFi check in
// loop() happens in the gap. It gives up waiting for such a gap after
// kMaxOverrunMs past the requested duration (very high rate signals).
static const uint32_t kReturnMarginUs = 300;
static const uint32_t kMaxOverrunMs = 20;

static const unsigned long MSTATUS_MIE_BIT = 0x8;

static inline uint32_t IRAM_ATTR cycles() {
  return esp_cpu_get_cycle_count();
}

// Single csrrci / csrrs instructions: far cheaper than portENTER_CRITICAL,
// so the idle poll loop can turn interrupts off for every sample.
static inline unsigned long IRAM_ATTR irqDisable() {
  return RV_READ_MSTATUS_AND_DISABLE_INTR();
}

static inline void IRAM_ATTR irqRestore(unsigned long mstatus) {
  if (mstatus & MSTATUS_MIE_BIT) {
    RV_SET_CSR(mstatus, MSTATUS_MIE_BIT);
  }
}

static inline bool IRAM_ATTR reached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

void SignalPassthrough::begin(uint8_t inputPin, uint8_t outputPin) {
  inMask = 1UL << inputPin;
  outMask = 1UL << outputPin;
  cpuMhz = getCpuFrequencyMhz();
  idleCycles = kIdleUs * cpuMhz;
  maxBurstCycles = kMaxBurstUs * cpuMhz;

  const gpio_num_t gpio = (gpio_num_t)inputPin;
  gpio_reset_pin(gpio);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  // No receiver connected reads as low, i.e. no signal for the ESC.
  gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);

  calibrate();
}

// Measures the normal time between two samples of the idle poll loop in
// run() (same instruction sequence, interrupts off so nothing interferes).
void IRAM_ATTR SignalPassthrough::calibrate() {
  uint32_t longest = 0;
  uint32_t prev = cycles();
  volatile bool sink = false;
  for (int i = 0; i < 2000; i++) {
    const unsigned long irq = irqDisable();
    const uint32_t now = cycles();
    sink = REG_READ(GPIO_IN_REG) & inMask;
    if (i > 0 && now - prev > longest) {
      longest = now - prev;
    }
    prev = now;
    irqRestore(irq);
  }
  (void)sink;
  // The run() loop also does the schedule bookkeeping per sample; allow for
  // that on top of the measured bare loop.
  maxGapCycles = 2 * longest + (kGapSlackNs * cpuMhz) / 1000;
}

// Learns the signal period from the rising edges of forwarded pulses.
void IRAM_ATTR SignalPassthrough::notePulse(uint32_t rise) {
  if (haveLastRise) {
    const uint32_t d = rise - lastRise;
    const uint32_t tol = period / 16;
    if (period != 0 && d + tol >= period && d <= period + tol) {
      period += (int32_t)(d - period) / 4;  // follow slow drift
      if (stable < 255) {
        stable++;
      }
    } else if (period != 0 && d > period + tol) {
      // A dropped / missed pulse in between: fine if d is a small multiple.
      const uint32_t k = (d + period / 2) / period;
      const uint32_t err = d > k * period ? d - k * period : k * period - d;
      if (k > 4 || err > tol) {
        period = 0;
        stable = 0;
      }
    } else {
      period = 0;
      stable = 0;
    }
    if (period == 0 && d >= kMinPeriodUs * cpuMhz && d <= kMaxPeriodUs * cpuMhz) {
      period = d;  // new candidate, needs kStableNeeded confirmations
    }
    if (period != 0) {
      guard = period / 16;
      if (guard < kMinGuardUs * cpuMhz) guard = kMinGuardUs * cpuMhz;
      if (guard > kMaxGuardUs * cpuMhz) guard = kMaxGuardUs * cpuMhz;
    }
  }
  haveLastRise = true;
  lastRise = rise;
  nextExpected = rise + period;
  misses = 0;
}

// Forwards one burst. Entered with interrupts off and the output just set
// high. Returns true if it ended on an idle low.
bool IRAM_ATTR SignalPassthrough::mirrorBurst() {
  const uint32_t start = cycles();
  uint32_t lastHigh = start;
  bool level = true;
  while (true) {
    const bool in = REG_READ(GPIO_IN_REG) & inMask;
    if (in != level) {
      REG_WRITE(in ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, outMask);
      level = in;
    }
    const uint32_t now = cycles();
    if (level) {
      lastHigh = now;
    } else if (now - lastHigh >= idleCycles) {
      return true;
    }
    if (now - start >= maxBurstCycles) {
      REG_WRITE(GPIO_OUT_W1TC_REG, outMask);  // never leave the output stuck high
      return !level;
    }
  }
}

void IRAM_ATTR SignalPassthrough::run(uint32_t durationMs) {
  const uint32_t start = cycles();
  const uint32_t end = start + durationMs * 1000 * cpuMhz;
  const uint32_t hardEnd = end + kMaxOverrunMs * 1000 * cpuMhz;
  const uint32_t returnMargin = kReturnMarginUs * cpuMhz;

  // Output only ever goes high for a validated burst.
  REG_WRITE(GPIO_OUT_W1TC_REG, outMask);
  REG_WRITE(GPIO_ENABLE_W1TS_REG, outMask);

  // The input counts as idle once consecutive samples, none further apart
  // than maxGapCycles, have seen it low for idleCycles. Anything that breaks
  // that chain (an interrupt or task switch while sampling) means an edge may
  // have been missed or seen late.
  bool inLow = false;
  uint32_t lowStart = 0;
  uint32_t lastLow = 0;
  // Start of the current low period counted across interrupt gaps (only a
  // high sample resets it). Not good enough to forward on, but good enough
  // to tell a burst start from a DShot bit for period learning.
  uint32_t looseLowStart = 0;

  bool held = false;           // interrupts being kept off across samples
  unsigned long savedIrq = 0;  // mstatus from before we took them

  while (true) {
    if (!held) {
      savedIrq = irqDisable();
    }
    uint32_t now = cycles();

    if (!(REG_READ(GPIO_IN_REG) & inMask)) {
      if (!inLow) {
        lowStart = looseLowStart = now;
      } else if (now - lastLow > maxGapCycles) {
        lowStart = now;  // interrupted: restart the verified-idle chain
      }
      inLow = true;
      lastLow = now;
    } else {
      // Input high. Forward only if the rising edge was seen promptly (the
      // previous low sample was just one normal loop pass ago) after a
      // verified idle period, i.e. at the start of a pulse / DShot frame.
      const bool idleBefore = inLow && lastLow - lowStart >= idleCycles;
      if (idleBefore && now - lastLow <= maxGapCycles) {
        REG_WRITE(GPIO_OUT_W1TS_REG, outMask);
        const bool endedLow = mirrorBurst();
        forwarded++;
        notePulse(now);
        inLow = endedLow;
        lastLow = cycles();
        lowStart = looseLowStart = lastLow - idleCycles;  // mirrorBurst() ended on idleCycles of low
      } else {
        // Late edge, an edge less than idleCycles after an interrupt (the
        // idle before it cannot be verified), or the middle of a burst we did
        // not start forwarding: the output stays low until the input has
        // been idle again.
        if (inLow && lastLow - looseLowStart >= idleCycles) {
          dropped++;
          // Still a burst start, its time known to within one interrupt:
          // far inside the period tolerance and the guard. Learning the
          // period from dropped pulses matters when the signal sits right
          // on top of a periodic interrupt: then every pulse is dropped and
          // the schedule that moves the interrupts away would never lock on.
          notePulse(now);
        }
        inLow = false;
      }
      now = cycles();
    }

    // Decide whether interrupts may run before the next sample.
    bool hold = false;
    if (stable >= kStableNeeded) {
      // Step past expected pulses that never came.
      while (reached(now, nextExpected + guard)) {
        nextExpected += period;
        if (++misses > kMaxMisses) {
          stable = 0;
          break;
        }
      }
      hold = stable >= kStableNeeded && reached(now, nextExpected - guard);
    }

    if (hold) {
      held = true;
      continue;
    }
    held = false;
    irqRestore(savedIrq);  // pending interrupts run here, in the gap

    if (reached(now, end)) {
      const bool gapAhead = stable < kStableNeeded ||
                            !reached(now + returnMargin, nextExpected - guard);
      if (gapAhead || reached(now, hardEnd)) {
        return;
      }
    }
  }
}

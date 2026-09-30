#include "signal_passthrough.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "riscv/csr.h"
#include "soc/gpio_reg.h"

// A burst (one pulse, or one whole DShot frame) ends once the input has been
// low for kIdleUs, longer than any low time inside a DShot150 frame. A
// forwarded burst runs entirely with interrupts off, so it is either
// forwarded intact or dropped as a whole.
static const uint32_t kIdleUs = 20;

// Upper bound on one interrupts-off burst (e.g. input stuck high). Longer
// than any valid servo pulse, well under the 300ms interrupt watchdog.
// A dropped burst is waited out with interrupts enabled.
static const uint32_t kMaxBurstUs = 5000;

// Extra slack allowed on top of the calibrated sample-to-sample time before
// a rising edge is treated as possibly late. Any interrupt handler takes
// longer than this.
static const uint32_t kGapSlackNs = 250;

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
  maxGapCycles = longest + (kGapSlackNs * cpuMhz) / 1000;
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
  const uint32_t end = cycles() + durationMs * 1000 * cpuMhz;

  // Output only ever goes high for a validated burst.
  REG_WRITE(GPIO_OUT_W1TC_REG, outMask);
  REG_WRITE(GPIO_ENABLE_W1TS_REG, outMask);

  // The input counts as idle once consecutive samples, none further apart
  // than maxGapCycles, have seen it low for idleCycles. Anything that breaks
  // that chain (an interrupt or task switch while waiting) means an edge may
  // have been missed or seen late.
  bool inLow = false;
  uint32_t lowStart = 0;
  uint32_t lastLow = 0;

  while ((int32_t)(cycles() - end) < 0) {
    const unsigned long irq = irqDisable();
    const uint32_t now = cycles();

    if (!(REG_READ(GPIO_IN_REG) & inMask)) {
      if (!inLow || now - lastLow > maxGapCycles) {
        lowStart = now;  // (re)start the verified-idle chain
      }
      inLow = true;
      lastLow = now;
      irqRestore(irq);  // pending interrupts run here, between samples
      continue;
    }

    // Input high. Forward only if the rising edge was seen promptly (the
    // previous low sample was just one normal loop pass ago) after a
    // verified idle period, i.e. at the start of a pulse / DShot frame.
    const bool idleBefore = inLow && lastLow - lowStart >= idleCycles;
    if (idleBefore && now - lastLow <= maxGapCycles) {
      REG_WRITE(GPIO_OUT_W1TS_REG, outMask);
      const bool endedLow = mirrorBurst();
      forwarded++;
      // mirrorBurst() only returns after idleCycles of low, so the input is
      // already verified idle.
      inLow = endedLow;
      lastLow = cycles();
      lowStart = lastLow - idleCycles;
    } else {
      // Late edge, or the middle of a burst we did not start forwarding:
      // keep the output low (with interrupts enabled) until the input has
      // been idle again.
      if (idleBefore) {
        dropped++;
      }
      inLow = false;
    }
    irqRestore(irq);
  }
}

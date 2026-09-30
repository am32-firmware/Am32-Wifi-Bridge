#include "am32_single_wire.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"

// Two levels of protection keep the bit timing intact:
//  * The scheduler is suspended for a whole packet and the wait for its reply,
//    so the WiFi / TCP tasks (which run at a much higher priority than loop())
//    can never stall us for milliseconds in the middle of a packet. The
//    bootloader drops a packet if the gap between two bytes exceeds 5 bit
//    times (~260us).
//  * Interrupts are disabled for a single transmitted byte (~520us), a
//    received frame (<= ~45ms for the largest 83 byte read), or a 1ms polling
//    window while waiting for the first start bit. Interrupt handlers only
//    run in between and are short enough not to matter.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const uint32_t kPollWindowUs = 1000;

static inline uint32_t IRAM_ATTR cycles() {
  return esp_cpu_get_cycle_count();
}

static inline void IRAM_ATTR waitUntil(uint32_t target) {
  while ((int32_t)(cycles() - target) < 0) {
  }
}

static inline bool IRAM_ATTR expired(uint32_t deadline) {
  return (int32_t)(cycles() - deadline) >= 0;
}

void SingleWireSerial::begin(uint8_t pin, uint32_t baud) {
  mask = 1UL << pin;
  cpuMhz = getCpuFrequencyMhz();
  const uint64_t cpuHz = (uint64_t)cpuMhz * 1000000ULL;

  for (int k = 0; k <= 10; k++) {
    edgeOffset[k] = (uint32_t)(((uint64_t)k * cpuHz) / baud);
  }
  for (int k = 0; k < 10; k++) {
    sampleOffset[k] = (uint32_t)(((uint64_t)(2 * k + 1) * cpuHz) / (2ULL * baud));
  }

  const gpio_num_t gpio = (gpio_num_t)pin;
  gpio_reset_pin(gpio);
  gpio_set_level(gpio, 1);
  // Input stays enabled permanently; output is switched on and off through
  // the GPIO_ENABLE register so a direction change costs a single write.
  gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
  driveIdleHigh();
}

void IRAM_ATTR SingleWireSerial::driveIdleHigh() {
  REG_WRITE(GPIO_OUT_W1TS_REG, mask);
  REG_WRITE(GPIO_ENABLE_W1TS_REG, mask);
}

void IRAM_ATTR SingleWireSerial::release() {
  REG_WRITE(GPIO_ENABLE_W1TC_REG, mask);
}

void IRAM_ATTR SingleWireSerial::writeBytes(const uint8_t *data, size_t len) {
  driveIdleHigh();
  // Each byte gets its own critical section; with the scheduler suspended
  // only short interrupt handlers can run between bytes.
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    portENTER_CRITICAL(&s_mux);
    const uint32_t start = cycles();
    REG_WRITE(GPIO_OUT_W1TC_REG, mask);  // start bit
    for (int bit = 0; bit < 8; bit++) {
      waitUntil(start + edgeOffset[bit + 1]);
      if (b & 0x01) {
        REG_WRITE(GPIO_OUT_W1TS_REG, mask);
      } else {
        REG_WRITE(GPIO_OUT_W1TC_REG, mask);
      }
      b >>= 1;
    }
    waitUntil(start + edgeOffset[9]);
    REG_WRITE(GPIO_OUT_W1TS_REG, mask);  // stop bit
    waitUntil(start + edgeOffset[10]);
    portEXIT_CRITICAL(&s_mux);
  }
  // Hand the line to the ESC; it answers no sooner than 5 bit times later.
  release();
}

size_t IRAM_ATTR SingleWireSerial::readFrameLocked(uint8_t *buf, size_t maxLen,
                                                   uint32_t edge,
                                                   uint32_t interByteCycles) {
  size_t n = 0;
  while (n < maxLen) {
    waitUntil(edge + sampleOffset[0]);
    if (REG_READ(GPIO_IN_REG) & mask) {
      framingError = true;  // start bit should still be low
      break;
    }
    uint8_t b = 0;
    for (int bit = 0; bit < 8; bit++) {
      waitUntil(edge + sampleOffset[bit + 1]);
      if (REG_READ(GPIO_IN_REG) & mask) {
        b |= (uint8_t)(1U << bit);
      }
    }
    waitUntil(edge + sampleOffset[9]);
    if (!(REG_READ(GPIO_IN_REG) & mask)) {
      framingError = true;  // stop bit should be high
      break;
    }
    buf[n++] = b;

    // The bootloader sends bytes back to back, so the next start bit follows
    // half a bit after the stop-bit sample. Stay in the critical section.
    const uint32_t limit = cycles() + interByteCycles;
    bool gotEdge = false;
    while (!expired(limit)) {
      if (!(REG_READ(GPIO_IN_REG) & mask)) {
        edge = cycles();
        gotEdge = true;
        break;
      }
    }
    if (!gotEdge) {
      break;
    }
  }
  return n;
}

size_t IRAM_ATTR SingleWireSerial::readBytes(uint8_t *buf, size_t maxLen,
                                             uint32_t firstByteTimeoutUs,
                                             uint32_t interByteTimeoutUs) {
  release();
  framingError = false;
  if (maxLen == 0) {
    return 0;
  }

  const uint32_t deadline = cycles() + firstByteTimeoutUs * cpuMhz;
  const uint32_t interByteCycles = interByteTimeoutUs * cpuMhz;
  bool sawIdleHigh = false;
  size_t n = 0;

  while (true) {
    bool started = false;
    portENTER_CRITICAL(&s_mux);
    const uint32_t windowEnd = cycles() + kPollWindowUs * cpuMhz;
    while (!expired(windowEnd) && !expired(deadline)) {
      const bool high = REG_READ(GPIO_IN_REG) & mask;
      if (high) {
        sawIdleHigh = true;
      } else if (sawIdleHigh) {
        // falling edge of a start bit (only counted after the line was idle)
        const uint32_t edge = cycles();
        started = true;
        n = readFrameLocked(buf, maxLen, edge, interByteCycles);
        break;
      }
    }
    portEXIT_CRITICAL(&s_mux);

    if (started || expired(deadline)) {
      break;
    }
  }

  driveIdleHigh();
  return n;
}

void SingleWireSerial::write(const uint8_t *data, size_t len) {
  vTaskSuspendAll();
  writeBytes(data, len);
  xTaskResumeAll();
}

size_t SingleWireSerial::read(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
                              uint32_t interByteTimeoutUs) {
  vTaskSuspendAll();
  const size_t n = readBytes(buf, maxLen, firstByteTimeoutUs, interByteTimeoutUs);
  xTaskResumeAll();
  return n;
}

size_t SingleWireSerial::transfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxMax,
                                  uint32_t firstByteTimeoutUs, uint32_t interByteTimeoutUs) {
  vTaskSuspendAll();
  writeBytes(tx, txLen);
  const size_t n = readBytes(rx, rxMax, firstByteTimeoutUs, interByteTimeoutUs);
  xTaskResumeAll();
  return n;
}

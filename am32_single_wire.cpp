#include "am32_single_wire.h"

#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"

// UART1 is free on the ESP32-C3: the Arduino log uses USB CDC or UART0.
static const uart_port_t kPort = UART_NUM_1;

// Ring buffer for received bytes (must be larger than the 128 byte FIFO) and
// event queue depth. With an RX interrupt per byte there is one data event
// per byte, so the queue is sized for the largest 259 byte reply; error
// events are what we are after.
static const int kRxBufferSize = 1024;
static const int kEventQueueSize = 300;

// Converts a timeout to RTOS ticks, adding one tick for tick granularity and
// one for the RX interrupt to move the byte into the ring buffer.
static TickType_t ticksFor(uint32_t us) {
  return pdMS_TO_TICKS((us + 999) / 1000) + 2;
}

void SingleWireSerial::begin(uint8_t pin, uint32_t baud) {
  this->pin = pin;
  this->baud = baud;

  uart_config_t cfg = {};
  cfg.baud_rate = (int)baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  uart_driver_install(kPort, kRxBufferSize, 0, kEventQueueSize, &events, 0);
  uart_param_config(kPort, &cfg);
  // Hand every byte to the driver as soon as it is received, so the
  // inter-byte timeout in readBytes() measures the real gap on the wire.
  uart_set_rx_full_threshold(kPort, 1);
  uart_set_rx_timeout(kPort, 2);

  const gpio_num_t gpio = (gpio_num_t)pin;
  gpio_reset_pin(gpio);
  gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
  driveIdleHigh();
}

void SingleWireSerial::driveIdleHigh() {
  const gpio_num_t gpio = (gpio_num_t)pin;
  // The UART sees an idle line while the passthrough owns the pin.
  esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, U1RXD_IN_IDX, false);
  gpio_set_level(gpio, 1);
  // Still open-drain here, so switching the output to the GPIO register
  // (which is high) keeps the line released until it is made push-pull.
  esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
}

// Hands the pin to the UART as open-drain, with stale bytes and events from
// any earlier transaction discarded.
void SingleWireSerial::attach() {
  const gpio_num_t gpio = (gpio_num_t)pin;
  gpio_set_level(gpio, 1);
  gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT_OD);  // high = released
  esp_rom_gpio_connect_out_signal(pin, U1TXD_OUT_IDX, false, false);  // TX idles high
  uart_flush_input(kPort);
  if (events) {
    xQueueReset(events);
  }
  framingError = false;
  echoError = false;
  esp_rom_gpio_connect_in_signal(pin, U1RXD_IN_IDX, false);
}

void SingleWireSerial::checkEvents() {
  if (!events) {
    return;
  }
  uart_event_t ev;
  while (xQueueReceive(events, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case UART_FRAME_ERR:
      case UART_PARITY_ERR:
      case UART_BREAK:
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        framingError = true;
        break;
      default:
        break;
    }
  }
}

void SingleWireSerial::writeBytes(const uint8_t *data, size_t len) {
  const uint32_t byteUs = 10UL * 1000000UL / baud;
  uart_write_bytes(kPort, data, len);
  uart_wait_tx_done(kPort, ticksFor(byteUs * (len + 2)));

  // RX hears everything we send on the shared line. Consume that echo so the
  // reply starts at the beginning of the receive buffer, and check it.
  for (size_t i = 0; i < len; i++) {
    uint8_t b;
    if (uart_read_bytes(kPort, &b, 1, ticksFor(byteUs)) != 1) {
      echoError = true;
      break;
    }
    if (b != data[i]) {
      echoError = true;
    }
  }
}

size_t SingleWireSerial::readBytes(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
                                   uint32_t interByteTimeoutUs) {
  size_t n = 0;
  TickType_t timeout = ticksFor(firstByteTimeoutUs);
  while (n < maxLen) {
    if (uart_read_bytes(kPort, buf + n, 1, timeout) != 1) {
      break;
    }
    n++;
    timeout = ticksFor(interByteTimeoutUs);
  }
  checkEvents();
  return n;
}

void SingleWireSerial::write(const uint8_t *data, size_t len) {
  attach();
  writeBytes(data, len);
  checkEvents();
  driveIdleHigh();
}

size_t SingleWireSerial::read(uint8_t *buf, size_t maxLen, uint32_t firstByteTimeoutUs,
                              uint32_t interByteTimeoutUs) {
  attach();
  const size_t n = readBytes(buf, maxLen, firstByteTimeoutUs, interByteTimeoutUs);
  driveIdleHigh();
  return n;
}

size_t SingleWireSerial::transfer(const uint8_t *tx, size_t txLen, uint8_t *rx, size_t rxMax,
                                  uint32_t firstByteTimeoutUs, uint32_t interByteTimeoutUs) {
  attach();
  writeBytes(tx, txLen);
  const size_t n = readBytes(rx, rxMax, firstByteTimeoutUs, interByteTimeoutUs);
  driveIdleHigh();
  return n;
}

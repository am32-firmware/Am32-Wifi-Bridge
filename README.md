# AM32 ESP32-C3 Web Configurator

An ESP32-C3 Arduino sketch that replaces the Offline-Configurator's
**USB / Arduino Connect** (direct) mode. The ESP32-C3 talks to the AM32
bootloader directly over the ESC signal wire and serves the configuration UI
over WiFi, so all you need is a phone or laptop browser.

## Wiring

| ESP32-C3            | ESC          |
|---------------------|--------------|
| GPIO 4 (`ESC_SIGNAL_PIN`) | signal |
| GND                 | ground       |
| GPIO 6 (`PASSTHROUGH_INPUT_PIN`) | receiver / FC signal (optional, pulled down) |

No diode, resistor or TX/RX bridge is needed. The single pin is switched
between output and input in software. A 220-1k series resistor is optional
protection.

## Signal passthrough

While **no device is connected to the WiFi**, the sketch copies GPIO 6 to the
ESC signal pin in a tight register-polling loop, about 0.1-0.2 us latency.
With nothing on GPIO 6, the pull-down keeps the ESC signal low, so an ESC
powered in this state starts its firmware normally.

* **Pulses that might be corrupted are dropped, not forwarded.** Between
  pulses, the loop turns interrupts off only for each single sample
  (`csrrci`/`csrrs` on `mstatus`, a couple of instructions), and it
  timestamps every sample with the cycle counter. A rising edge is forwarded
  only when both of these are true:
  * the previous sample saw the input low no more than one normal loop pass
    earlier. The pass time is calibrated at boot, plus 250 ns slack. This
    proves the edge wasn't seen late because an interrupt or the WiFi task
    ran.
  * the input had been verifiably low for 20 us before the edge, so it is the
    start of a pulse or DShot frame and not the middle of one.

  Otherwise the output stays low until the input has been idle again, so the
  whole pulse or DShot frame is skipped. For PWM that means the ESC keeps its
  previous value for one frame. For DShot one frame is missed.
* A forwarded pulse or frame runs entirely with interrupts off (at most
  5 ms), so its width is copied exactly. A pulse longer than 5 ms (input
  stuck high) is cut off and the output driven low.
* Mirroring runs in 50 ms slices. Between slices the sketch checks whether
  someone has joined the WiFi. A pulse already in progress when a slice
  starts is dropped.
* The USB serial log reports forwarded/dropped counts every 5 s when drops
  happened.
* Works for PWM/servo, Oneshot, Multishot and normal DShot. **Bidirectional
  DShot does not work**, because the ESC's telemetry reply can't travel back
  through a one-way copy.
* When a phone or laptop joins the WiFi, the passthrough stops and the signal
  line is held **high** for the configurator. An ESC firmware that is already
  running sees that as signal loss (no pulses). Power-cycle the ESC to get
  into the bootloader.
* If `STA_SSID` is set and the ESP is joined to that network, that also counts
  as "WiFi connected", so the passthrough stays off.

## Configuring

Join the WiFi **first**, then power the ESC. The ESP then holds the signal
line high, so the bootloader's `checkForSignal()` stays in the bootloader
instead of starting the firmware.

## Build

* Arduino IDE with the ESP32 core 3.x, board **ESP32C3 Dev Module** (or your
  C3 board). Enable *USB CDC On Boot* to see the serial log.
* Settings are at the top of `AM32_ESP32C3_WebConfig.ino`: signal pin, AP
  name and password, optional station network, and a TX power limit that
  many C3 SuperMini boards need.

## Use

1. Join WiFi `AM32-Config` (password `am32config`) and open
   <http://192.168.4.1> (or <http://am32.local>).
2. **Connect & read settings**: sends the BLHeli boot-init, reads the device
   info, then reads the firmware name and the 48 byte settings block.
3. Edit settings and **Write settings to ESC**, or save/load a `.bin` config
   file. **Edit offline** works without an ESC, like the Qt tool's Offline Mode.
4. **Flash firmware** accepts `.bin` or `.hex`. The flow matches the Qt tool:
   it clears the settings boot byte, writes 128 byte chunks with up to 8
   retries each, then restores the boot byte. If the ESC has no settings yet,
   it sends the default EEPROM and starts the ESC instead.
5. **Exit bootloader** starts the ESC firmware. Power-cycle the ESC to connect
   again.

## Files

| File | Purpose |
|------|---------|
| `am32_single_wire.*` | Bit-banged 19200 8N1 half-duplex serial on one GPIO |
| `am32_bootloader.*`  | Port of `BF_ROOTLOADER`, `connectMotor()`, `readInitData()` and `sendDirect()` |
| `signal_passthrough.*` | GPIO 6 -> GPIO 4 passthrough loop while WiFi is idle |
| `web_page.h`         | UI; the settings table mirrors `Widget::eepromFieldTable()` and `defaults.h` |
| `AM32_ESP32C3_WebConfig.ino` | WiFi, web server and JSON API |

### HTTP API

| Endpoint | Body | Result |
|----------|------|--------|
| `POST /api/connect` | – | device info, addresses, name, `eeprom` (hex) |
| `POST /api/eeprom` | hex bytes | writes to the settings address |
| `POST /api/flash?offset=N` | hex bytes (<= 256) | writes at firmware start + N |
| `POST /api/reset` | – | CMD_RUN |
| `GET /api/status` | – | connection state |

## Timing notes

The FreeRTOS scheduler is suspended for each packet plus the wait for its
reply, so the higher-priority WiFi/TCP tasks cannot stall a packet mid-way.
The bootloader drops a packet if two bytes are more than ~260 us apart.
Interrupts are disabled for one transmitted byte at a time (~520 us), for a
received frame (<= ~45 ms), or for 1 ms polling windows while waiting for a
reply. Bit timing uses the CPU cycle counter.

Packets are 20 ms apart, and each reply timeout is at least as long as the Qt
tool's `sendDirect()` wait for the same step. Write failures report the step
that failed and the bootloader's reply (`0xC1` bad command, `0xC2` bad CRC)
in the web log and on the USB serial port.

## Differences from the Qt direct mode

* The settings read uses bootloader magic address `0x21` (EEPROM - 32) when
  the bootloader protocol is 2 or higher. The Qt tool's `eeprom_address - 32`
  points 128 bytes too low on 128k parts with shifted addresses.
* No echo stripping is needed, because the ESP never hears its own
  transmission.
* A final firmware chunk is padded to a multiple of 8 bytes with `0xFF`.
* Music upload is not included. The Qt direct mode never reads the music
  area either.

// AM32 ESC web configurator for the ESP32-C3.
//
// Replaces the Offline-Configurator's "USB / Arduino Connect" (direct) mode:
// the ESP32-C3 talks to the AM32 bootloader itself over the ESC signal wire
// (bit-banged 19200 8N1, one pin switched between output and input) and
// serves the configuration UI over WiFi.
//
// While no device is connected to the WiFi, the ESP32-C3 is a signal
// passthrough: whatever arrives on PASSTHROUGH_INPUT_PIN (receiver / flight
// controller) is copied to the ESC signal pin. As soon as a phone or laptop
// joins the WiFi, the passthrough stops and the signal pin is held high for
// the configurator.
//
// Wiring: ESC signal -> ESC_SIGNAL_PIN, ESC ground -> ESP32-C3 GND,
// receiver signal -> PASSTHROUGH_INPUT_PIN (optional).
// To configure: join the WiFi first, then power the ESC so the bootloader
// sees the signal line held high and stays in the bootloader.
//
// Board: "ESP32C3 Dev Module" (arduino-esp32 3.x). Enable "USB CDC On Boot"
// to see the log on the USB serial port.

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "am32_bootloader.h"
#include "am32_single_wire.h"
#include "signal_passthrough.h"
#include "web_page.h"

// ---- configuration --------------------------------------------------------
#define ESC_SIGNAL_PIN 4
#define ESC_BAUD 19200

// Receiver / flight controller signal input (internal pull-down).
#define PASSTHROUGH_INPUT_PIN 6

// Access point the ESP32-C3 always creates (password must be 8+ characters).
#define AP_SSID "AM32-Config"
#define AP_PASSWORD "am32config"

// Optionally also join an existing network (leave empty to disable).
#define STA_SSID ""
#define STA_PASSWORD ""

#define MDNS_NAME "am32"  // http://am32.local

// Many ESP32-C3 "SuperMini" boards cannot hold a WiFi link at full TX power.
#define LIMIT_WIFI_TX_POWER 1
// ---------------------------------------------------------------------------

static SingleWireSerial escWire;
static Am32Bootloader esc(escWire);
static WebServer server(80);
static SignalPassthrough passthrough;
static bool passthroughActive = false;

static String toHex(const uint8_t *buf, size_t len) {
  static const char digits[] = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += digits[buf[i] >> 4];
    s += digits[buf[i] & 0x0F];
  }
  return s;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns the decoded length, or -1 for malformed / oversized input.
static int fromHex(const String &hex, uint8_t *out, size_t maxLen) {
  if (hex.length() % 2 != 0 || hex.length() / 2 > maxLen) {
    return -1;
  }
  for (size_t i = 0; i < hex.length() / 2; i++) {
    const int hi = hexNibble(hex[2 * i]);
    const int lo = hexNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return -1;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return hex.length() / 2;
}

static String jsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

static void sendResult(bool ok) {
  String json = "{\"ok\":";
  json += ok ? "true" : "false";
  if (!ok) {
    json += ",\"error\":\"" + jsonEscape(esc.lastError) + "\"";
  }
  json += "}";
  server.send(200, "application/json", json);
}

static void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// Widget::connectMotor() in direct mode: boot init, then read name + settings.
static void handleConnect() {
  uint8_t name[32];
  uint8_t eeprom[48];
  const bool ok = esc.connect() && esc.readSettings(name, eeprom);
  if (!ok) {
    Serial.printf("connect failed: %s\n", esc.lastError.c_str());
    sendResult(false);
    return;
  }

  String fwName;
  for (int i = 0; i < 32 && name[i] >= 0x20 && name[i] < 0x7F; i++) {
    fwName += (char)name[i];
  }

  String json = "{\"ok\":true";
  json += ",\"deviceInfo\":\"" + toHex(esc.deviceInfo, sizeof(esc.deviceInfo)) + "\"";
  json += ",\"flashCode\":" + String(esc.deviceInfo[4]);
  json += ",\"protocol\":" + String(esc.protocolVersion);
  json += ",\"eepromAddress\":" + String(esc.eepromAddress);
  json += ",\"firmwareStart\":" + String(esc.firmwareStart);
  json += ",\"firmwareAreaSize\":" + String(esc.firmwareAreaSize());
  json += ",\"addressShift\":" + String(esc.addressShift ? "true" : "false");
  json += ",\"name\":\"" + jsonEscape(fwName) + "\"";
  json += ",\"eeprom\":\"" + toHex(eeprom, sizeof(eeprom)) + "\"";
  json += "}";
  Serial.printf("connected, flash code 0x%02x, bootloader protocol %u\n", esc.deviceInfo[4],
                esc.protocolVersion);
  server.send(200, "application/json", json);
}

static void handleWriteEeprom() {
  uint8_t buf[256];
  const int len = fromHex(server.arg("plain"), buf, sizeof(buf));
  if (len <= 0) {
    esc.lastError = "Bad EEPROM data";
    sendResult(false);
    return;
  }
  sendResult(esc.writeEeprom(buf, len));
}

static void handleFlashChunk() {
  uint8_t buf[256];
  const int len = fromHex(server.arg("plain"), buf, sizeof(buf));
  if (len <= 0 || !server.hasArg("offset")) {
    esc.lastError = "Bad chunk";
    sendResult(false);
    return;
  }
  const uint32_t offset = strtoul(server.arg("offset").c_str(), nullptr, 10);
  sendResult(esc.writeFirmwareChunk(offset, buf, len));
}

static void handleReset() {
  esc.runApplication();
  sendResult(true);
}

static void handleStatus() {
  String json = "{\"connected\":";
  json += esc.connected ? "true" : "false";
  json += ",\"pin\":" + String(ESC_SIGNAL_PIN) + "}";
  server.send(200, "application/json", json);
}

static void startWifi() {
  const bool useSta = strlen(STA_SSID) > 0;
  WiFi.mode(useSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
#if LIMIT_WIFI_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
  Serial.printf("AP \"%s\" at http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (useSta) {
    WiFi.begin(STA_SSID, STA_PASSWORD);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("joined \"%s\" at http://%s\n", STA_SSID, WiFi.localIP().toString().c_str());
    } else {
      Serial.println("could not join station network, AP only");
    }
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }
}

void setup() {
  // Hold the signal line high first so an ESC powered together with us
  // stays in its bootloader.
  escWire.begin(ESC_SIGNAL_PIN, ESC_BAUD);
  passthrough.begin(PASSTHROUGH_INPUT_PIN, ESC_SIGNAL_PIN);

  Serial.begin(115200);
  startWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/connect", HTTP_POST, handleConnect);
  server.on("/api/eeprom", HTTP_POST, handleWriteEeprom);
  server.on("/api/flash", HTTP_POST, handleFlashChunk);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// "WiFi connected" means someone can reach the configurator: a device has
// joined our access point, or we have joined the configured station network.
static bool wifiInUse() {
  if (WiFi.softAPgetStationNum() > 0) {
    return true;
  }
  return strlen(STA_SSID) > 0 && WiFi.status() == WL_CONNECTED;
}

void loop() {
  if (!wifiInUse()) {
    if (!passthroughActive) {
      passthroughActive = true;
      esc.connected = false;  // the line is no longer ours
      Serial.println("WiFi idle: signal passthrough on");
    }
    // Mirror the input in 50ms slices, then re-check the WiFi.
    passthrough.run(50);

    // Report pulses dropped because an interrupt may have delayed them.
    static uint32_t lastReport = 0;
    static uint32_t lastDropped = 0;
    if (millis() - lastReport > 5000) {
      lastReport = millis();
      if (passthrough.droppedPulses() != lastDropped) {
        lastDropped = passthrough.droppedPulses();
        Serial.printf("passthrough: %lu forwarded, %lu dropped\n",
                      (unsigned long)passthrough.forwardedPulses(), (unsigned long)lastDropped);
      }
    }
    return;
  }

  if (passthroughActive) {
    passthroughActive = false;
    escWire.driveIdleHigh();
    Serial.println("WiFi client connected: passthrough off, signal line held high");
  }
  server.handleClient();
  delay(1);
}

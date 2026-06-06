#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"

#include "config.h"

namespace {

Adafruit_NeoPixel rgb(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

uint8_t currentR = 0;
uint8_t currentG = 0;
uint8_t currentB = 0;
bool lightOn = false;
bool isStationMode = false;
bool cameraEnabled = false;
String ipAddress = "0.0.0.0";

// Lightweight in-memory logs for debugging from the web UI.
static constexpr int LOG_CAPACITY = 40;
String logLines[LOG_CAPACITY] = {};
int logHead = 0;
int logCount = 0;

void appendLog(const String& line) {
  String entry = String(millis()) + "ms " + line;
  logLines[logHead] = entry;
  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) logCount++;
  Serial.println(entry);
}

// FC UART state
HardwareSerial fcSerial(1); // UART1

// RC — sent continuously at RC_SEND_HZ; ESP32 is always the RC source.
static constexpr uint32_t RC_SEND_HZ     = 20;   // packets per second
static constexpr uint32_t RC_SEND_PERIOD = 1000 / RC_SEND_HZ;
uint8_t  rcPayload[32]      = {};     // current channels (16×uint16_le); safe defaults set in setup()
unsigned long rcLastSentMs  = 0;

// Motor test — entire sequence runs in loop() so RC never pauses
enum class MotorTestPhase : uint8_t { IDLE, ARMING, RAMPING, REDUCING, DISARMING };
MotorTestPhase motorTestPhase    = MotorTestPhase::IDLE;
unsigned long  motorTestStartMs  = 0;
unsigned long  motorTestDisarmMs = 0;  // millis() of last disarm — enforces Betaflight rearm lockout
bool           cachedArmed       = false; // last known armed state from MSP_STATUS

static constexpr uint32_t MT_ARM_MS         =  500; // hold arm signal before ramping
static constexpr uint32_t MT_RAMP_MS        = 3000; // motors at 1300 µs
static constexpr uint32_t MT_REDUCE_MS      =  300; // throttle back to 1000 before disarm
static constexpr uint32_t MT_DISARM_MS      =  300; // hold disarm signal
static constexpr uint32_t REARM_COOLDOWN_MS = 6000; // Betaflight ~5 s disarm lockout

static void setChannel(int idx, uint16_t val) {
  rcPayload[idx * 2]     = val & 0xFF;
  rcPayload[idx * 2 + 1] = (val >> 8) & 0xFF;
}

static const char* motorTestPhaseName() {
  switch (motorTestPhase) {
    case MotorTestPhase::ARMING:    return "arming";
    case MotorTestPhase::RAMPING:   return "ramping";
    case MotorTestPhase::REDUCING:  return "reducing";
    case MotorTestPhase::DISARMING: return "disarming";
    default:                        return "idle";
  }
}

// ─── LED ─────────────────────────────────────────────────────────────────────

void applyColor(uint8_t r, uint8_t g, uint8_t b) {
  currentR = r;
  currentG = g;
  currentB = b;
  lightOn = (r > 0 || g > 0 || b > 0);
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
  Serial.printf("[LED] r=%d g=%d b=%d  on=%s\n", r, g, b, lightOn ? "true" : "false");
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

String buildLightJson() {
  JsonDocument doc;
  doc["ok"]      = true;
  doc["lightOn"] = lightOn;
  doc["r"]       = currentR;
  doc["g"]       = currentG;
  doc["b"]       = currentB;
  String out;
  serializeJson(doc, out);
  return out;
}
// ─── FC UART / MSP ─────────────────────────────────────────────────────────────────────
//
// Protocol: MSP (MultiWii Serial Protocol) — used by Betaflight for telemetry
// and RC control. Betaflight setup required before this works:
//   1. Ports tab → enable MSP on the UART you wired to ESP32, baud 115200.
//   2. CLI →  set msp_override_channels = 31   (ch 1-5: roll/pitch/throttle/yaw/AUX1)
//             save
//

static constexpr uint32_t MSP_TIMEOUT_MS = 300;
static constexpr uint8_t  MSP_STATUS     = 101;
static constexpr uint8_t  MSP_ATTITUDE   = 108;
static constexpr uint8_t  MSP_ANALOG     = 110;
static constexpr uint8_t  MSP_SET_RAW_RC = 200;

static void mspSend(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  uint8_t chk = len ^ cmd;
  for (uint8_t i = 0; i < len; i++) chk ^= payload[i];
  fcSerial.write('$'); fcSerial.write('M'); fcSerial.write('<');
  fcSerial.write(len); fcSerial.write(cmd);
  if (len > 0) fcSerial.write(payload, len);
  fcSerial.write(chk);
  fcSerial.flush();
}

enum ParserState {
  S_HEADER_DOLLAR,
  S_HEADER_M,
  S_HEADER_ARROW,
  S_SIZE,
  S_CMD,
  S_PAYLOAD,
  S_CHECKSUM
};

ParserState parserState = S_HEADER_DOLLAR;
uint8_t rxSize = 0;
uint8_t rxCmd = 0;
uint8_t rxBuf[64] = {};
uint8_t rxCount = 0;
uint8_t rxChecksum = 0;

// Global telemetry variables
bool fcArmed = false;
float fcRoll = 0.0f;
float fcPitch = 0.0f;
float fcYaw = 0.0f;
float fcVbat = 0.0f;
float fcAmps = 0.0f;
unsigned long fcLastResponseMs = 0;

void handleMspPacket(uint8_t cmd, const uint8_t* buf, uint8_t len) {
  fcLastResponseMs = millis();
  
  if (cmd == MSP_STATUS && len >= 11) {
    uint32_t flags = (uint32_t)buf[6] | ((uint32_t)buf[7] << 8)
                   | ((uint32_t)buf[8] << 16) | ((uint32_t)buf[9] << 24);
    fcArmed = (flags & 1u) != 0;
    cachedArmed = fcArmed;
  }
  else if (cmd == MSP_ATTITUDE && len >= 6) {
    int16_t roll  = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    int16_t pitch = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    int16_t yaw   = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    fcRoll  = roll  / 10.0f;
    fcPitch = pitch / 10.0f;
    fcYaw   = (float)yaw;
  }
  else if (cmd == MSP_ANALOG && len >= 7) {
    fcVbat = buf[0] / 10.0f;
    int16_t amp  = (int16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8));
    fcAmps  = amp / 100.0f;
  }
}

void parseMspByte(uint8_t b) {
  switch (parserState) {
    case S_HEADER_DOLLAR:
      if (b == '$') parserState = S_HEADER_M;
      break;
    case S_HEADER_M:
      if (b == 'M') parserState = S_HEADER_ARROW;
      else parserState = S_HEADER_DOLLAR;
      break;
    case S_HEADER_ARROW:
      if (b == '>') parserState = S_SIZE;
      else parserState = S_HEADER_DOLLAR;
      break;
    case S_SIZE:
      rxSize = b;
      rxChecksum = b;
      rxCount = 0;
      parserState = S_CMD;
      break;
    case S_CMD:
      rxCmd = b;
      rxChecksum ^= b;
      if (rxSize == 0) {
        parserState = S_CHECKSUM;
      } else if (rxSize > sizeof(rxBuf)) {
        parserState = S_HEADER_DOLLAR;
      } else {
        parserState = S_PAYLOAD;
      }
      break;
    case S_PAYLOAD:
      rxBuf[rxCount] = b;
      rxChecksum ^= b;
      rxCount++;
      if (rxCount >= rxSize) {
        parserState = S_CHECKSUM;
      }
      break;
    case S_CHECKSUM:
      if (b == rxChecksum) {
        handleMspPacket(rxCmd, rxBuf, rxSize);
      }
      parserState = S_HEADER_DOLLAR;
      break;
  }
}

void handleFcTelemetry() {
  Serial.println("[HTTP] GET /api/v1/fc/telemetry");
  JsonDocument doc;
  doc["ok"] = true;
  doc["motorTest"] = motorTestPhaseName();
  doc["rcThr"] = (uint16_t)rcPayload[4] | ((uint16_t)rcPayload[5] << 8);
  doc["rcAux1"] = (uint16_t)rcPayload[8] | ((uint16_t)rcPayload[9] << 8);
  doc["rearmCooldownMs"] = (motorTestDisarmMs > 0 && millis() - motorTestDisarmMs < REARM_COOLDOWN_MS)
                              ? (REARM_COOLDOWN_MS - (millis() - motorTestDisarmMs))
                              : 0;

  if (millis() - fcLastResponseMs < 3000) {
    doc["armed"] = fcArmed;
    doc["roll"]  = fcRoll;
    doc["pitch"] = fcPitch;
    doc["yaw"]   = fcYaw;
    doc["vbatV"] = fcVbat;
    doc["ampA"]  = fcAmps;
  } else {
    doc["armed"]   = false;
    doc["fcError"] = "No response from FC — check UART wiring and Betaflight port config";
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLogs() {
  JsonDocument doc;
  doc["ok"] = true;
  JsonArray arr = doc["logs"].to<JsonArray>();
  for (int i = 0; i < logCount; i++) {
    int idx = (logHead - logCount + i + LOG_CAPACITY) % LOG_CAPACITY;
    arr.add(logLines[idx]);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleFcRcSet() {
  Serial.println("[HTTP] POST /api/v1/fc/rc");
  String body = server.arg("plain");

  JsonDocument req;
  if (deserializeJson(req, body) || !req["channels"].is<JsonArray>()) {
    server.send(400, "application/json", R"json({"ok":false,"error":"Expected {channels:[...]} array (16 values 1000-2000)"})json");
    return;
  }

  JsonArray arr = req["channels"].as<JsonArray>();
  for (int i = 0; i < 16; i++) {
    uint16_t val = (i < (int)arr.size())
                   ? (uint16_t)constrain((int)arr[i], 1000, 2000)
                   : (i == 2 ? 1000u : 1500u);
    setChannel(i, val);
  }

  uint16_t thr = (uint16_t)rcPayload[4] | ((uint16_t)rcPayload[5] << 8);
  uint16_t aux = (uint16_t)rcPayload[8] | ((uint16_t)rcPayload[9] << 8);
  appendLog("RC update thr=" + String(thr) + " aux1=" + String(aux));
  server.send(200, "application/json", R"({"ok":true})");
}

void handleFcArm() {
  Serial.println("[HTTP] POST /api/v1/fc/arm");
  String body = server.arg("plain");

  JsonDocument req;
  if (deserializeJson(req, body) || !req["arm"].is<bool>()) {
    server.send(400, "application/json", R"json({"ok":false,"error":"Expected {arm:true/false}"})json");
    return;
  }
  bool arm = req["arm"].as<bool>();

  if (arm) {
    for (int i = 0; i < 16; i++) {
      if (i == 2)      setChannel(i, 1000u); // throttle low
      else if (i == 4) setChannel(i, 1500u); // AUX1 = arm
      else             setChannel(i, 1500u); // others center
    }
    appendLog("ARM command aux1=1500 thr=1000");
  } else {
    for (int i = 0; i < 16; i++) {
      if (i == 4)      setChannel(i, 1000u); // AUX1 = disarm
      else if (i == 2) setChannel(i, 1000u); // throttle low
      else             setChannel(i, 1500u); // others center
    }
    appendLog("DISARM command aux1=1000 thr=1000");
  }
  server.send(200, "application/json", R"({"ok":true})");
}

void handleFcRcStop() {
  Serial.println("[HTTP] POST /api/v1/fc/rc/stop");
  if (motorTestPhase != MotorTestPhase::IDLE) {
    motorTestPhase   = MotorTestPhase::IDLE;
    motorTestDisarmMs = millis();
    appendLog("Motor test aborted by rc/stop");
  }
  for (int i = 0; i < 16; i++) setChannel(i, (i == 2 || i == 4) ? 1000u : 1500u);
  appendLog("RC reset safe (thr=1000 aux1=1000)");
  server.send(200, "application/json", R"({"ok":true})");
}

void handleFcMotorTest() {
  Serial.println("[HTTP] POST /api/v1/fc/motor-test");
  if (motorTestPhase != MotorTestPhase::IDLE) {
    appendLog("Motor test start rejected: already running");
    server.send(409, "application/json", R"json({"ok":false,"error":"Motor test already running"})json");
    return;
  }
  if (motorTestDisarmMs > 0 && millis() - motorTestDisarmMs < REARM_COOLDOWN_MS) {
    appendLog("Motor test start rejected: rearm cooldown");
    server.send(429, "application/json", R"json({"ok":false,"error":"Please wait ~6 s before re-arming (Betaflight rearm lockout)"})json");
    return;
  }
  for (int i = 0; i < 16; i++) setChannel(i, 1500u);
  setChannel(2, 1000u); // throttle low
  setChannel(4, 1500u); // AUX1 arm position
  motorTestPhase   = MotorTestPhase::ARMING;
  motorTestStartMs = millis();
  cachedArmed      = false;
  appendLog("Motor test start -> ARMING");
  server.send(202, "application/json", R"({"ok":true,"phase":"arming"})");
}
// ─── Camera ───────────────────────────────────────────────────────────────────

bool initCamera() {
  camera_config_t cfg = {};
  cfg.pin_pwdn     = CAM_PWDN;
  cfg.pin_reset    = CAM_RESET;
  cfg.pin_xclk     = CAM_XCLK;
  cfg.pin_sccb_sda = CAM_SIOD;
  cfg.pin_sccb_scl = CAM_SIOC;
  cfg.pin_d7 = CAM_D7; cfg.pin_d6 = CAM_D6; cfg.pin_d5 = CAM_D5;
  cfg.pin_d4 = CAM_D4; cfg.pin_d3 = CAM_D3; cfg.pin_d2 = CAM_D2;
  cfg.pin_d1 = CAM_D1; cfg.pin_d0 = CAM_D0;
  cfg.pin_vsync    = CAM_VSYNC;
  cfg.pin_href     = CAM_HREF;
  cfg.pin_pclk     = CAM_PCLK;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    cfg.frame_size   = FRAMESIZE_SVGA;
    cfg.jpeg_quality = 10;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    Serial.println("[CAM] PSRAM found — SVGA, q=10, fb=2");
  } else {
    cfg.frame_size   = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
    Serial.println("[CAM] No PSRAM — QVGA, q=12, fb=1");
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init failed: 0x%x\n", err);
    return false;
  }

  // OV3660: flip vertically — sensor is typically mounted upside-down
  sensor_t *s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 0); }

  Serial.println("[CAM] Initialized OK");
  return true;
}

void handleCameraStatus() {
  Serial.println("[HTTP] GET /api/v1/camera/status");
  JsonDocument doc;
  doc["ok"]      = true;
  doc["running"] = cameraEnabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleCameraStart() {
  Serial.println("[HTTP] POST /api/v1/camera/start");
  if (cameraEnabled) {
    server.send(200, "application/json", R"({"ok":true,"running":true})");
    return;
  }
  if (!initCamera()) {
    server.send(500, "application/json", R"({"ok":false,"error":"Camera init failed"})");
    return;
  }
  cameraEnabled = true;
  server.send(200, "application/json", R"({"ok":true,"running":true})");
}

void handleCameraStop() {
  Serial.println("[HTTP] POST /api/v1/camera/stop");
  if (cameraEnabled) {
    esp_camera_deinit();
    cameraEnabled = false;
    Serial.println("[CAM] Deinitialized");
  }
  server.send(200, "application/json", R"({"ok":true,"running":false})");
}

void handleCameraSnapshot() {
  Serial.println("[HTTP] GET /api/v1/camera/snapshot");
  if (!cameraEnabled) {
    server.send(503, "application/json", R"({"ok":false,"error":"Camera not running"})");
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "application/json", R"({"ok":false,"error":"Frame capture failed"})");
    return;
  }
  WiFiClient client = server.client();
  client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\nCache-Control: no-cache\r\n\r\n", fb->len);
  client.write(fb->buf, fb->len);
  Serial.printf("[CAM] Snapshot sent (%d bytes)\n", fb->len);
  esp_camera_fb_return(fb);
}

void handleCameraStream() {
  Serial.println("[HTTP] GET /api/v1/camera/stream");
  if (!cameraEnabled) {
    server.send(503, "application/json", R"({"ok":false,"error":"Camera not running"})");
    return;
  }
  WiFiClient client = server.client();
  client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
  Serial.println("[CAM] Stream started");
  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] Frame capture failed"); break; }
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    delay(0); // yield to watchdog
  }
  Serial.println("[CAM] Stream ended");
}

// ─── Route handlers ──────────────────────────────────────────────────────────

void handleHealth() {
  JsonDocument doc;
  doc["ok"]          = true;
  doc["uptimeMs"]    = millis();
  doc["wifiRssiDbm"] = WiFi.RSSI();
  doc["lightOn"]     = lightOn;
  doc["r"]           = currentR;
  doc["g"]           = currentG;
  doc["b"]           = currentB;
  doc["mode"]        = isStationMode ? "station" : "access-point";
  doc["ip"]          = ipAddress;
  doc["cameraOn"]    = cameraEnabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleLightStatus() {
  Serial.println("[HTTP] GET /api/v1/light/status");
  server.send(200, "application/json", buildLightJson());
}

void handleLightColor() {
  Serial.println("[HTTP] POST /api/v1/light/color");
  String body = server.arg("plain");
  Serial.printf("[HTTP] Body: %s\n", body.c_str());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[HTTP] JSON parse error: %s\n", err.c_str());
    server.send(400, "application/json", R"({"ok":false,"error":"Invalid JSON body"})");
    return;
  }

  uint8_t r = doc["r"] | 0;
  uint8_t g = doc["g"] | 0;
  uint8_t b = doc["b"] | 0;
  applyColor(r, g, b);
  server.send(200, "application/json", buildLightJson());
}

void handleLightStop() {
  Serial.println("[HTTP] POST /api/v1/light/stop");
  applyColor(0, 0, 0);
  server.send(200, "application/json", buildLightJson());
}

void handleFutureUart() {}

void handleNotFound() {
  Serial.printf("[HTTP] 404 %s %s\n",
                server.method() == HTTP_GET ? "GET" : "POST",
                server.uri().c_str());
  server.send(404, "application/json", R"({"ok":false,"error":"Not found"})");
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────

void connectWifi() {
  Serial.printf("[WiFi] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    isStationMode = true;
    ipAddress = WiFi.localIP().toString();
    Serial.printf("[WiFi] Connected. IP: %s\n", ipAddress.c_str());
    return;
  }

  Serial.println("[WiFi] Station failed — starting AP fallback");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  isStationMode = false;
  ipAddress = WiFi.softAPIP().toString();
  Serial.printf("[WiFi] AP started. SSID: %s  IP: %s\n", AP_SSID, ipAddress.c_str());
}

void registerRoutes() {
  server.on("/api/v1/health",           HTTP_GET,  handleHealth);
  server.on("/api/v1/logs",             HTTP_GET,  handleLogs);
  server.on("/api/v1/light/status",     HTTP_GET,  handleLightStatus);
  server.on("/api/v1/light/color",      HTTP_POST, handleLightColor);
  server.on("/api/v1/light/stop",       HTTP_POST, handleLightStop);
  server.on("/api/v1/camera/status",    HTTP_GET,  handleCameraStatus);
  server.on("/api/v1/camera/start",     HTTP_POST, handleCameraStart);
  server.on("/api/v1/camera/stop",      HTTP_POST, handleCameraStop);
  server.on("/api/v1/camera/snapshot",  HTTP_GET,  handleCameraSnapshot);
  server.on("/api/v1/camera/stream",    HTTP_GET,  handleCameraStream);
  server.on("/api/v1/fc/telemetry",     HTTP_GET,  handleFcTelemetry);
  server.on("/api/v1/fc/rc",            HTTP_POST, handleFcRcSet);
  server.on("/api/v1/fc/rc/stop",       HTTP_POST, handleFcRcStop);
  server.on("/api/v1/fc/arm",           HTTP_POST, handleFcArm);
  server.on("/api/v1/fc/motor-test",    HTTP_POST, handleFcMotorTest);
  server.onNotFound(handleNotFound);
  Serial.println("[HTTP] Routes registered");
}

}  // namespace

// ─── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] ESP32 starting");

  rgb.begin();
  rgb.setBrightness(80);
  rgb.show();
  Serial.printf("[LED] NeoPixel on pin %d count=%d\n", RGB_LED_PIN, RGB_LED_COUNT);

  // Brief blue flash to confirm firmware booted.
  applyColor(0, 0, 80);
  delay(400);
  applyColor(0, 0, 0);

  connectWifi();
  fcSerial.begin(115200, SERIAL_8N1, FC_RX_PIN_VALUE, FC_TX_PIN_VALUE);
  Serial.printf("[FC] UART1 started — TX=GPIO%d RX=GPIO%d @ 115200\n", FC_TX_PIN_VALUE, FC_RX_PIN_VALUE);
  appendLog("Boot complete, FC UART initialized");
  // Initialise RC payload: throttle low (ch2=1000), AUX1 disarm (ch4=1000), others center.
  for (int i = 0; i < 16; i++) {
    uint16_t v = (i == 2 || i == 4) ? 1000u : 1500u;
    rcPayload[i * 2]     = v & 0xFF;
    rcPayload[i * 2 + 1] = (v >> 8) & 0xFF;
  }
  registerRoutes();
  server.begin();
  Serial.println("[HTTP] Server listening on port 80");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Motor test state machine — advances between RC sends, never blocks.
  if (motorTestPhase != MotorTestPhase::IDLE) {
    unsigned long elapsed = now - motorTestStartMs;
    switch (motorTestPhase) {
      case MotorTestPhase::ARMING:
        if (elapsed >= MT_ARM_MS) {
          setChannel(2, 1300u);
          motorTestPhase   = MotorTestPhase::RAMPING;
          motorTestStartMs = now;
          appendLog("Motor test -> RAMPING thr=1300");
        }
        break;
      case MotorTestPhase::RAMPING:
        if (elapsed >= MT_RAMP_MS) {
          setChannel(2, 1000u);
          motorTestPhase   = MotorTestPhase::REDUCING;
          motorTestStartMs = now;
          appendLog("Motor test -> REDUCING thr=1000");
        }
        break;
      case MotorTestPhase::REDUCING:
        if (elapsed >= MT_REDUCE_MS) {
          setChannel(4, 1000u); // disarm AUX1
          motorTestPhase   = MotorTestPhase::DISARMING;
          motorTestStartMs = now;
          appendLog("Motor test -> DISARMING aux1=1000");
        }
        break;
      case MotorTestPhase::DISARMING:
        if (elapsed >= MT_DISARM_MS) {
          motorTestPhase    = MotorTestPhase::IDLE;
          motorTestDisarmMs = now;
          appendLog("Motor test complete");
        }
        break;
      default: break;
    }
  }

  // Send RC at RC_SEND_HZ continuously — ESP32 is always the RC source.
  if (now - rcLastSentMs >= RC_SEND_PERIOD) {
    mspSend(MSP_SET_RAW_RC, rcPayload, 32);
    rcLastSentMs = now;
  }

  // Request telemetry in round-robin every 400ms without blocking.
  static unsigned long lastRequestMs = 0;
  static uint8_t requestTick = 0;
  if (now - lastRequestMs >= 400) {
    lastRequestMs = now;
    if (requestTick == 0)      mspSend(MSP_STATUS, nullptr, 0);
    else if (requestTick == 1) mspSend(MSP_ATTITUDE, nullptr, 0);
    else if (requestTick == 2) mspSend(MSP_ANALOG, nullptr, 0);
    requestTick = (requestTick + 1) % 3;
  }

  // Continuous parsing of incoming FC bytes
  while (fcSerial.available()) {
    parseMspByte(fcSerial.read());
  }
}

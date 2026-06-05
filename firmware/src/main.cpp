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

void handleFutureUart() {
  Serial.println("[HTTP] POST /api/v1/fc/uart (not implemented)");
  server.send(501, "application/json",
              R"({"ok":false,"error":"UART control not implemented yet"})");
}

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
  server.on("/api/v1/light/status",     HTTP_GET,  handleLightStatus);
  server.on("/api/v1/light/color",      HTTP_POST, handleLightColor);
  server.on("/api/v1/light/stop",       HTTP_POST, handleLightStop);
  server.on("/api/v1/camera/status",    HTTP_GET,  handleCameraStatus);
  server.on("/api/v1/camera/start",     HTTP_POST, handleCameraStart);
  server.on("/api/v1/camera/stop",      HTTP_POST, handleCameraStop);
  server.on("/api/v1/camera/snapshot",  HTTP_GET,  handleCameraSnapshot);
  server.on("/api/v1/camera/stream",    HTTP_GET,  handleCameraStream);
  server.on("/api/v1/fc/uart",          HTTP_POST, handleFutureUart);
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
  registerRoutes();
  server.begin();
  Serial.println("[HTTP] Server listening on port 80");
}

void loop() {
  server.handleClient();
}

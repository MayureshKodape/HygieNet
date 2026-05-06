// ─── Terra Spray ESP32-S2 — PRODUCTION READY (millis timer) ───────────────
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ─── Pins ─────────────────────────────────────────────────────────────────
#define MOTOR_PIN   35
// ─── WebSocket Server ─────────────────────────────────────────────────────
const char*    WS_HOST = "**************";
const uint16_t WS_PORT = ********;
const char*    WS_PATH = "/";
// ─── WiFi AP ──────────────────────────────────────────────────────────────
const char* AP_NAME     = "TerraSpray";
const char* AP_PASSWORD = "12345678";
// ─── Settings ─────────────────────────────────────────────────────────────
const unsigned long TIMER_DURATION  = 20000;   // 20 seconds in milliseconds
const unsigned long SPRAY_DURATION  = 3000;    // 3 seconds spray
const unsigned long PING_INTERVAL   = 15000;   // ping every 15 seconds
// ─── Objects ──────────────────────────────────────────────────────────────
WebSocketsClient webSocket;
WiFiManager      wifiManager;
// ─── State variables ──────────────────────────────────────────────────────
bool          personInside  = false;
bool          timerRunning  = false;
bool          sprayActive   = false;
unsigned long entryTimeMs   = 0;   // millis() when person entered
unsigned long sprayStartMs  = 0;
unsigned long lastPingMs    = 0;
unsigned long lastPrintMs   = 0;

// Send JSON status to server
void sendStatus(String event, String detail) {
  if (!webSocket.isConnected()) return;
  StaticJsonDocument<200> doc;
  doc["device_id"] = "SPRAY1";
  doc["event"]     = event;
  doc["detail"]    = detail;
  doc["timestamp"] = millis();
  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
  Serial.println("[WS] Sent: " + json);
}

// Person entered washroom
void onPersonEntered() {
  personInside = true;
  timerRunning = true;
  sprayActive  = false;
  entryTimeMs  = millis();   // record entry time in milliseconds
  lastPrintMs  = 0;
  Serial.printf("[TIMER] Person entered! Timer started: %lu ms\n", TIMER_DURATION);
  sendStatus("TIMER", "started");
}

// ══════════════════════════════════════════════════════════════════════════
// Person left washroom
// ══════════════════════════════════════════════════════════════════════════
void onPersonLeft() {
  personInside = false;
  timerRunning = false;
  sprayActive  = false;
  digitalWrite(MOTOR_PIN, LOW);
  Serial.println("[TIMER] Person left — timer cancelled!");
  sendStatus("TIMER", "cancelled");
}

// ══════════════════════════════════════════════════════════════════════════
// Start spray
// ══════════════════════════════════════════════════════════════════════════
void startSpray() {
  Serial.println(">>>       SPRAYING NOW!              <<<");
  sendStatus("SPRAY", "started");
  digitalWrite(MOTOR_PIN, HIGH);
  sprayActive  = true;
  sprayStartMs = millis();
  timerRunning = false;
}

// ══════════════════════════════════════════════════════════════════════════
// Stop spray
// ══════════════════════════════════════════════════════════════════════════
void stopSpray() {
  digitalWrite(MOTOR_PIN, LOW);
  sprayActive  = false;
  personInside = false;
  Serial.println(">>>       SPRAY DONE!                <<<");
  sendStatus("SPRAY", "done");
}

// ══════════════════════════════════════════════════════════════════════════
// WebSocket events
// ══════════════════════════════════════════════════════════════════════════
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.println("[WS] Connected to server!");
      webSocket.sendTXT("{\"type\":\"bot\"}");
      Serial.println("[WS] Sent bot registration!");
      lastPingMs = millis();
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected! Auto-reconnecting...");
      break;

    case WStype_TEXT: {
      Serial.printf("[WS] Received: %s\n", payload);

      StaticJsonDocument<200> doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        Serial.println("[WS] JSON parse error — skipping");
        return;
      }

      String state    = doc["state"]     | "";
      String deviceId = doc["device_id"] | "";

      // Only listen to washroom device
      if (deviceId != "VEGG1") return;

      if (state == "CLOSED") {
        if (!timerRunning && !sprayActive) {
          onPersonEntered();
        }
      }
      else if (state == "OPEN") {
        if (timerRunning || sprayActive) {
          onPersonLeft();
        }
      }
      break;
    }

    default:
      break;
  }
}

// SETUP
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);

  // WiFi
  WiFi.setSleep(false);
  Serial.println("[WiFi] Connecting...");
  bool connected = wifiManager.autoConnect(AP_NAME, AP_PASSWORD);
  if (!connected) {
    Serial.println("[WiFi] Failed — rebooting!");
    delay(1000);
    ESP.restart();
  }
  Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString());

  // WebSocket
  webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);

  Serial.println("[BOOT] Terra Spray PRODUCTION ready!");
  Serial.printf("[BOOT] Timer set to: %lu seconds\n", TIMER_DURATION / 1000);
  Serial.printf("[BOOT] Spray duration: %lu seconds\n", SPRAY_DURATION / 1000);
}

// LOOP
void loop() {

  // 1. WebSocket tick
  webSocket.loop();

  // 2. Ping every 15 sec
  if (webSocket.isConnected()) {
    if (millis() - lastPingMs >= PING_INTERVAL) {
      lastPingMs = millis();
      webSocket.sendTXT("{\"type\":\"ping\"}");
      Serial.println("[WS] Ping sent");
    }
  }

  // 3. Stop spray when duration done
  if (sprayActive) {
    unsigned long sprayPassed = millis() - sprayStartMs;
    if (sprayPassed >= SPRAY_DURATION) {
      stopSpray();
    }
    return;
  }

  // 4. Check timer using millis()
  if (personInside && timerRunning) {
    unsigned long passed    = millis() - entryTimeMs;
    unsigned long remaining = (passed >= TIMER_DURATION) ? 0 : (TIMER_DURATION - passed);

    // Print every 1 second
    if (millis() - lastPrintMs >= 1000) {
      lastPrintMs = millis();
      Serial.printf("[TIMER] %lu sec passed | %lu sec remaining\n",
        passed / 1000, remaining / 1000);
    }

    // Time is up — SPRAY!
    if (passed >= TIMER_DURATION) {
      Serial.println("[TIMER] Time is UP! Spraying...");
      startSpray();
    }
  }
}

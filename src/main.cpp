#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <WakeOnLan.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <esp_system.h>

#include "env.h"

constexpr int LED_PIN = 38;
constexpr int POLL_INTERVAL_MS = 2500;
constexpr int ERROR_RESTART_MS = 30000;

enum LedState { LED_CONNECTING, LED_IDLE, LED_WOL_SENT, LED_ERROR_WIFI, LED_ERROR_REQUEST };
LedState ledState = LED_CONNECTING;
SemaphoreHandle_t ledMutex = xSemaphoreCreateMutex();

WiFiUDP UDP;
WakeOnLan WOL(UDP);
String lastMessageId = "";

Adafruit_NeoPixel strip(1, LED_PIN, NEO_RGB);

void setLedState(LedState state) {
  xSemaphoreTake(ledMutex, portMAX_DELAY);
  ledState = state;
  xSemaphoreGive(ledMutex);
}

LedState getLedState() {
  xSemaphoreTake(ledMutex, portMAX_DELAY);
  LedState state = ledState;
  xSemaphoreGive(ledMutex);
  return state;
}

void connectToWiFi() {
  setLedState(LED_CONNECTING);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  while (!WiFi.isConnected()) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected: " + WiFi.localIP().toString());
  setLedState(LED_IDLE);
}

String createHttpsRequest(const char* method, const String& path, const String& body = "") {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("discord.com", 443)) {
    setLedState(LED_ERROR_REQUEST);
    return "";
  }

  String req = String(method) + " " + path + " HTTP/1.1\r\n"
             + "Host: discord.com\r\n"
             + "Authorization: Bot " + BOT_TOKEN + "\r\n"
             + "Connection: close\r\n";

  if (body.length() > 0)
    req += "Content-Type: application/json\r\nContent-Length: " + String(body.length()) + "\r\n";
  req += "\r\n" + body;
  client.print(req);

  unsigned long waitTime = millis();
  while (!client.available() && millis() - waitTime < 5000) { // waits 5s for discord to respond
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (!client.available()) {
    setLedState(LED_ERROR_REQUEST);
    client.stop();
    return "";
  }

  while (client.available()) {
    if (client.readStringUntil('\n') == "\r") break;
  }

  String response = "";

  while (client.available()) {
    String sizeLine = client.readStringUntil('\n');
    sizeLine.trim();

    if (!sizeLine.length()) continue;

    unsigned long size = strtoul(sizeLine.c_str(), nullptr, 16);
    if (!size) break;

    unsigned long remaining = size;
    while (remaining > 0 && (client.available() || millis() - waitTime < 5000)) {
      if (client.available()) {
        response += static_cast<char>(client.read());
        remaining--;
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    client.readStringUntil('\n');
  }

  if (getLedState() == LED_ERROR_REQUEST) setLedState(LED_IDLE);
  client.stop();
  return response;
}

bool seedLastMessageId() {
  String response = createHttpsRequest("GET", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages?limit=1");
  DynamicJsonDocument doc(8192);

  if (!response.isEmpty() && !deserializeJson(doc, response) && doc.as<JsonArray>().size() > 0) {
    lastMessageId = doc[0]["id"].as<String>();
    Serial.println("Seeded: " + lastMessageId);
    return true;
  }
  return false;
}

[[noreturn]] void pollTask(void*) {
  while (getLedState() == LED_CONNECTING)
    vTaskDelay(pdMS_TO_TICKS(100));

  while (!seedLastMessageId()) {
    Serial.println("Failed to seed last message id, retrying in 5s...");
    if (!WiFi.isConnected())
      setLedState(LED_ERROR_WIFI);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  for (;;) {
    if (!WiFi.isConnected()) {
      setLedState(LED_ERROR_WIFI);
    } else {
      String response = createHttpsRequest("GET", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages?limit=1");

      if (!response.isEmpty()) {
        DynamicJsonDocument doc(8192);
        if (!deserializeJson(doc, response)) {
          JsonArray messages = doc.as<JsonArray>();
          bool triggered = false;

          for (size_t i = 0; i < messages.size(); i++) {
            String msgId   = messages[i]["id"].as<String>();
            String author  = messages[i]["author"]["id"].as<String>();
            String content = messages[i]["content"].as<String>();

            if (msgId > lastMessageId) lastMessageId = msgId;
            if (triggered || author != String(USER_ID)) continue;

            content.toLowerCase(); content.trim();
            if (content == "!wol") {
              triggered = true;
              WOL.sendMagicPacket(TARGET_PC_MAC);
              setLedState(LED_WOL_SENT);
              createHttpsRequest("POST", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages",
                R"json({"content":"Wake-up packet sent!"})json");
              createHttpsRequest("PUT", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages/" + msgId + "/reactions/%E2%9C%85/@me");
            }
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }
}

[[noreturn]] void ledTask(void*) {
  bool blinkOn = false;

  for (;;) {
    switch (getLedState()) {
      case LED_CONNECTING:
        strip.setPixelColor(0, Adafruit_NeoPixel::Color(0, 0, 255));
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(100));
        break;

      case LED_IDLE:
        strip.setPixelColor(0, Adafruit_NeoPixel::Color(0, 255, 0));
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(100));
        break;

      case LED_WOL_SENT:
        strip.setPixelColor(0, Adafruit_NeoPixel::Color(255, 255, 255));
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(500));
        setLedState(LED_IDLE);
        break;

      case LED_ERROR_WIFI:
        strip.setPixelColor(0, Adafruit_NeoPixel::Color(255, 0, 0));
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(100));
        break;

      case LED_ERROR_REQUEST:
        blinkOn = !blinkOn;
        strip.setPixelColor(0, blinkOn ? Adafruit_NeoPixel::Color(255, 0, 0) : Adafruit_NeoPixel::Color(0, 0, 0));
        strip.show();
        vTaskDelay(pdMS_TO_TICKS(150));
        break;
    }
  }
}

[[noreturn]] void watchdogTask(void*) {
  unsigned long errorStartMs = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool inError = (getLedState() == LED_ERROR_WIFI || getLedState() == LED_CONNECTING);

    if (!inError) {
      errorStartMs = 0;
    } else if (errorStartMs == 0)
      errorStartMs = millis();

    if (errorStartMs != 0 && millis() - errorStartMs >= ERROR_RESTART_MS)
      esp_restart();
  }
}

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.setBrightness(1);
  strip.show();

  for (int i = 0; i < 3; i++) {
    strip.setPixelColor(0, Adafruit_NeoPixel::Color(0, 255, 0));
    strip.show();
    delay(150);
    strip.clear();
    strip.show();
    delay(150);
  }

  xTaskCreate(pollTask, "poll", 16384, nullptr, 2, nullptr);
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  xTaskCreate(watchdogTask, "watchdog", 2048, nullptr, 1, nullptr);

  connectToWiFi();

  WOL.setRepeat(3, 100);
  WOL.calculateBroadcastAddress(WiFi.localIP(), WiFi.subnetMask());
}

void loop() {}
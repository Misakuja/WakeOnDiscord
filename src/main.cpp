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
constexpr int POLL_INTERVAL_MS = 1500;
constexpr int ERROR_RESTART_MS = 30000;

enum LedState { LED_CONNECTING, LED_IDLE, LED_WOL_SENT, LED_ERROR_WIFI, LED_ERROR_REQUEST };
volatile LedState ledState = LED_CONNECTING;

WiFiUDP UDP;
WakeOnLan WOL(UDP);
String lastMessageId = "0";

Adafruit_NeoPixel strip(1, LED_PIN, NEO_RGB);

void connectToWiFi() {
  ledState = LED_CONNECTING;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  while (!WiFi.isConnected()) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected: " + WiFi.localIP().toString());
  ledState = LED_IDLE;
}

String createHttpsRequest(const char* method, const String& path, const String& body = "") {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("discord.com", 443)) {
    ledState = LED_ERROR_REQUEST;
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
  while (!client.available() && millis() - waitTime < 5000); // waits 5s for discord to respond
  if (!client.available()) {
    ledState = LED_ERROR_REQUEST;
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

    while (size-- && (client.available() || millis() - waitTime < 5000))
      if (client.available()) response += static_cast<char>(client.read());
    client.readStringUntil('\n');
  }

  if (ledState == LED_ERROR_REQUEST) ledState = LED_IDLE;
  client.stop();
  return response;
}

void seedLastMessageId() {
  String response = createHttpsRequest("GET", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages?limit=1");
  DynamicJsonDocument doc(4096);
  if (!response.isEmpty() && !deserializeJson(doc, response) && doc.as<JsonArray>().size() > 0)
    lastMessageId = doc[0]["id"].as<String>();
  Serial.println("Seeded: " + lastMessageId);
}

[[noreturn]] void pollTask(void*) {
  while (ledState == LED_CONNECTING)
    vTaskDelay(pdMS_TO_TICKS(100));

  seedLastMessageId();

  for (;;) {
    if (!WiFi.isConnected()) {
      ledState = LED_ERROR_WIFI;
    } else {
      String response = createHttpsRequest("GET", "/api/v10/channels/" + String(CHANNEL_ID) + "/messages?limit=5&after=" + lastMessageId);

      if (!response.isEmpty()) {
        DynamicJsonDocument doc(4096);
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
              ledState = LED_WOL_SENT;
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
    switch (ledState) {
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
        ledState = LED_IDLE;
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
    bool inError = (ledState == LED_ERROR_WIFI || ledState == LED_ERROR_REQUEST || ledState == LED_CONNECTING);

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

  xTaskCreate(pollTask, "poll", 8192, nullptr, 1, nullptr);
  xTaskCreate(ledTask, "led", 2048, nullptr, 0, nullptr);
  xTaskCreate(watchdogTask, "watchdog", 2048, nullptr, 1, nullptr);

  connectToWiFi();

  WOL.setRepeat(3, 100);
  WOL.calculateBroadcastAddress(WiFi.localIP(), WiFi.subnetMask());
}

void loop() {}
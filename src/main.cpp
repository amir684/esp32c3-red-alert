#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <Preferences.h>

// ===================== PIN CONFIG =====================
#define NEOPIXEL_PIN    2     // GPIO2
#define NEOPIXEL_COUNT  12

#define MAX_DIN_PIN     7     // GPIO7  -> MAX7219 DIN
#define MAX_CLK_PIN     6     // GPIO6  -> MAX7219 CLK
#define MAX_CS_PIN      5     // GPIO5  -> MAX7219 CS
#define MAX_DEVICES     4     // 4 x 8x8 modules = 32x8

// ===================== CONSTANTS =====================
#define ALERT_URL      "https://www.oref.org.il/warningMessages/alert/Alerts.json"
#define CHECK_INTERVAL  3000UL   // Poll every 5 seconds
#define CAT_PRE_ALARM   10

// ===================== OBJECTS =====================
Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
MD_Parola matrix(MD_MAX72XX::FC16_HW, MAX_DIN_PIN, MAX_CLK_PIN, MAX_CS_PIN, MAX_DEVICES);
Preferences preferences;

// ===================== STATE =====================
enum AlertState { SAFE, PRE_ALARM, UNSAFE };
AlertState currentState = SAFE;

String cityName = "רמת השרון";   // default — overridden by saved value

// Flicker state for UNSAFE
unsigned long lastBlink   = 0;
bool          blinkOn     = false;

// Consecutive SAFE readings required before reverting to SAFE state
int safeConfirmCount = 0;
#define SAFE_CONFIRM_NEEDED 3

// ===================== URL DECODE =====================
// WiFiManager may return the Hebrew input URL-encoded (%D7%A8...)
String urlDecode(const String& s) {
    String out;
    char hex[3] = {0};
    for (unsigned int i = 0; i < s.length(); i++) {
        if (s[i] == '%' && i + 2 < s.length()) {
            hex[0] = s[i + 1];
            hex[1] = s[i + 2];
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ===================== NEOPIXEL =====================
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        pixel.setPixelColor(i, pixel.Color(r, g, b));
    }
    pixel.show();
}

// ===================== MATRIX =====================
void showStatic(const char* text) {
    matrix.displayClear();
    matrix.displayText(text, PA_CENTER, 0, 500, PA_PRINT, PA_NO_EFFECT);
    matrix.displayAnimate();
}

void showScroll(const char* text) {
    matrix.displayClear();
    matrix.displayScroll(text, PA_LEFT, PA_SCROLL_LEFT, 60);
}

// ===================== APPLY STATE =====================
void applyState(AlertState state) {
    switch (state) {
        case SAFE:
            setColor(0, 80, 0);       // Green
            showStatic("SAFE");
            break;

        case PRE_ALARM:
            setColor(255, 80, 0);     // Orange
            showScroll("PRE ALARM");
            break;

        case UNSAFE:
            blinkOn = true;
            setColor(80, 0, 0);       // Red (flicker handled in loop)
            showStatic("UNSAFE");
            break;
    }
}

// ===================== CHECK ALERTS =====================
void checkAlerts() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();   // No CA cert needed

    HTTPClient http;
    if (!http.begin(client, ALERT_URL)) return;

    http.addHeader("Referer",          "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Content-Type",     "application/json");
    http.setTimeout(8000);

    int code = http.GET();
    AlertState newState = SAFE;

    Serial.printf("[HTTP] code=%d\n", code);

    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();
        Serial.printf("[HTTP] payload len=%d\n", payload.length());
        Serial.println(payload);

        // Strip UTF-8 BOM (0xEF 0xBB 0xBF) — oref API includes it
        if (payload.length() >= 3 &&
            (uint8_t)payload[0] == 0xEF &&
            (uint8_t)payload[1] == 0xBB &&
            (uint8_t)payload[2] == 0xBF) {
            payload = payload.substring(3);
        }

        if (payload.length() > 10) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);

            if (err) {
                Serial.printf("[JSON] parse error: %s\n", err.c_str());
            } else {
                // cat comes as a string ("10") not int — convert explicitly
                int cat = atoi(doc["cat"].as<const char*>() ?: "0");
                String title = doc["title"].as<String>();
                Serial.printf("[JSON] cat=%d title=%s\n", cat, title.c_str());

                // "האירוע הסתיים" = end-of-event, treat as SAFE regardless of cat
                bool isEndOfEvent = title.indexOf("הסתיים") >= 0;

                if (!isEndOfEvent) {
                    JsonArray data = doc["data"].as<JsonArray>();
                    for (JsonVariant v : data) {
                        String area = v.as<String>();
                        Serial.printf("[JSON] area: %s\n", area.c_str());
                        if (area.indexOf(cityName) >= 0 || cityName.indexOf(area) >= 0) {
                            newState = (cat == CAT_PRE_ALARM) ? PRE_ALARM : UNSAFE;
                            Serial.printf("[Match] city found! cat=%d\n", cat);
                            break;
                        }
                    }
                } else {
                    Serial.println("[JSON] end-of-event message, ignoring");
                }
            }
        } else {
            Serial.println("[HTTP] empty response = no active alerts");
        }
    } else {
        Serial.printf("[HTTP] error: %s\n", http.errorToString(code).c_str());
    }
    http.end();

    // Require 3 consecutive SAFE readings before leaving alert state
    if (newState == SAFE && currentState != SAFE) {
        safeConfirmCount++;
        Serial.printf("[Alert] SAFE confirm %d/%d\n", safeConfirmCount, SAFE_CONFIRM_NEEDED);
        if (safeConfirmCount < SAFE_CONFIRM_NEEDED) return;
    } else {
        safeConfirmCount = 0;
    }

    if (newState != currentState) {
        currentState = newState;
        applyState(currentState);
        Serial.printf("[Alert] State -> %s\n",
            currentState == SAFE      ? "SAFE"      :
            currentState == PRE_ALARM ? "PRE_ALARM" : "UNSAFE");
    }
}

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);

    // --- NeoPixel init ---
    pixel.begin();
    pixel.setBrightness(200);
    setColor(0, 0, 80);   // Blue = booting

    // --- Matrix init ---
    matrix.begin();
    matrix.setIntensity(5);   // 0–15
    showStatic("...");

    // --- Load saved city ---
    preferences.begin("redalert", true);
    String saved = preferences.getString("city", "");
    preferences.end();
    if (saved.length() > 0) cityName = saved;
    Serial.printf("[Setup] City: %s\n", cityName.c_str());

    // --- WiFiManager ---
    WiFiManager wm;
    wm.setTitle("Red Alert Config");

    WiFiManagerParameter cityParam("city", "City Name (Hebrew)", cityName.c_str(), 80);
    wm.addParameter(&cityParam);

    wm.setSaveParamsCallback([&]() {
        String val = urlDecode(String(cityParam.getValue()));
        val.trim();
        if (val.length() > 0) {
            cityName = val;
            preferences.begin("redalert", false);
            preferences.putString("city", cityName);
            preferences.end();
            Serial.printf("[Setup] City saved: %s\n", cityName.c_str());
        }
    });

    showStatic("WiFi");
    wm.setConfigPortalTimeout(180);   // 3 min portal timeout → restart

    if (!wm.autoConnect("RedAlert-Setup")) {
        Serial.println("[Setup] Portal timeout — restarting");
        ESP.restart();
    }

    // Connected
    Serial.printf("[Setup] WiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());
    setColor(0, 80, 0);
    showStatic("OK!");
    delay(1500);

    applyState(SAFE);
}

// ===================== LOOP =====================
unsigned long lastCheck = 0;

void loop() {
    // Drive matrix animation — loop scroll only for PRE ALARM
    if (matrix.displayAnimate() && currentState == PRE_ALARM) {
        matrix.displayReset();
    }

    // Fast flicker NeoPixel red when UNSAFE
    if (currentState == UNSAFE && millis() - lastBlink >= 60) {
        lastBlink = millis();
        blinkOn = !blinkOn;
        blinkOn ? setColor(80, 0, 0) : setColor(0, 0, 0);
    }

    // Poll API
    if (millis() - lastCheck >= CHECK_INTERVAL) {
        lastCheck = millis();
        checkAlerts();
    }
}

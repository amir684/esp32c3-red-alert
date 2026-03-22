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

#define BUTTON_PIN      3     // GPIO3  -> config button (connect to GND)
#define LONG_PRESS_MS   3000

// ===================== CONSTANTS =====================
#define ALERT_URL       "https://www.oref.org.il/warningMessages/alert/Alerts.json"
#define CHECK_INTERVAL  3000UL
#define CAT_PRE_ALARM   10
#define SAFE_TIMEOUT_MS (20UL * 60 * 1000)
#define API_FAIL_MAX    5     // consecutive failures before NO API shown

// ===================== OBJECTS =====================
Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
MD_Parola matrix(MD_MAX72XX::FC16_HW, MAX_DIN_PIN, MAX_CLK_PIN, MAX_CS_PIN, MAX_DEVICES);
Preferences preferences;

// ===================== STATE =====================
enum AlertState { SAFE, PRE_ALARM, ALARM, UNSAFE, NO_API, BAD_CITY };

// Alert state — shared between tasks, protected by mutex
volatile AlertState currentState = SAFE;
SemaphoreHandle_t   stateMutex;

// Error flags — written by setup/alertTask, read by loop
volatile int  apiFailCount = 0;
volatile bool cityValid    = true;

String cityName = "רמת השרון";

// Flicker / blink state (loop task only)
unsigned long lastBlink = 0;
bool          blinkOn   = false;

// Last time our city appeared in an active alert (safety timeout)
unsigned long lastAlertTime = 0;

// ===================== URL DECODE =====================
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
// Must be called from loop task only (SPI/NeoPixel not thread-safe)
void applyState(AlertState state) {
    blinkOn = false;
    switch (state) {
        case SAFE:
            setColor(0, 80, 0);         // Green solid
            showStatic("SAFE");
            break;

        case PRE_ALARM:
            setColor(255, 80, 0);       // Orange solid
            showScroll("PRE ALARM");
            break;

        case ALARM:
            blinkOn = true;
            setColor(80, 0, 0);         // Red flicker (handled in loop)
            showStatic("ALARM");
            break;

        case UNSAFE:
            setColor(80, 0, 0);         // Red solid
            showStatic("UNSAFE");
            break;

        case NO_API:
            blinkOn = true;
            setColor(0, 0, 80);         // Blue blink (handled in loop)
            showScroll("NO API");
            break;

        case BAD_CITY:
            setColor(80, 0, 80);        // Magenta solid
            showScroll("BAD CITY");
            break;
    }
}

// cityValid starts true — set to false only if city never matches after first successful alert poll
// (validation via startup API call is unreliable due to Unicode escaping in cities endpoint)

// ===================== ALERT TASK (core 0) =====================
void alertTask(void* param) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL));

        if (WiFi.status() != WL_CONNECTED) continue;

        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        if (!http.begin(client, ALERT_URL)) {
            apiFailCount++;
            continue;
        }

        http.addHeader("Referer",          "https://www.oref.org.il/");
        http.addHeader("X-Requested-With", "XMLHttpRequest");
        http.addHeader("Content-Type",     "application/json");
        http.setTimeout(8000);

        int code = http.GET();

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        AlertState cur = currentState;
        xSemaphoreGive(stateMutex);

        AlertState newState = cur;

        if (code == HTTP_CODE_OK) {
            apiFailCount = 0;   // reset failure counter on success

            String payload = http.getString();
            payload.trim();

            // Strip UTF-8 BOM
            if (payload.length() >= 3 &&
                (uint8_t)payload[0] == 0xEF &&
                (uint8_t)payload[1] == 0xBB &&
                (uint8_t)payload[2] == 0xBF) {
                payload = payload.substring(3);
            }

            if (payload.length() > 10) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);

                if (!err) {
                    int cat = atoi(doc["cat"].as<const char*>() ?: "0");
                    String title = doc["title"].as<String>();
                    Serial.printf("[JSON] cat=%d title=%s\n", cat, title.c_str());

                    bool isEndOfEvent = title.indexOf("הסתיים") >= 0;
                    JsonArray data = doc["data"].as<JsonArray>();

                    bool cityFound = false;
                    for (JsonVariant v : data) {
                        String area = v.as<String>();
                        if (area.indexOf(cityName) >= 0 || cityName.indexOf(area) >= 0) {
                            cityFound = true;
                            break;
                        }
                    }

                    if (isEndOfEvent) {
                        if (cityFound) {
                            newState = SAFE;
                            Serial.println("[Alert] end-of-event -> SAFE");
                        }
                    } else {
                        if (cityFound) {
                            newState = (cat == CAT_PRE_ALARM) ? PRE_ALARM : ALARM;
                            lastAlertTime = millis();
                            Serial.printf("[Match] cat=%d -> %s\n", cat,
                                newState == PRE_ALARM ? "PRE_ALARM" : "ALARM");
                        } else if (cur == ALARM) {
                            newState = UNSAFE;
                            Serial.println("[Alert] ALARM ended -> UNSAFE");
                        }
                    }
                } else {
                    Serial.printf("[JSON] parse error: %s\n", err.c_str());
                }
            } else {
                if (cur == ALARM) {
                    newState = UNSAFE;
                    Serial.println("[HTTP] empty + was ALARM -> UNSAFE");
                }
            }
        } else {
            apiFailCount++;
            Serial.printf("[HTTP] error %d (fail count: %d)\n", code, (int)apiFailCount);
        }
        http.end();

        // Safety timeout
        if (cur != SAFE && millis() - lastAlertTime > SAFE_TIMEOUT_MS) {
            newState = SAFE;
            Serial.println("[Alert] safety timeout -> SAFE");
        }

        if (newState != cur) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            currentState = newState;
            xSemaphoreGive(stateMutex);
            Serial.printf("[Alert] State -> %s\n",
                newState == SAFE      ? "SAFE"      :
                newState == PRE_ALARM ? "PRE_ALARM" :
                newState == ALARM     ? "ALARM"     : "UNSAFE");
        }
    }
}

// ===================== SETUP =====================
void setup() {
    Serial.begin(115200);

    stateMutex = xSemaphoreCreateMutex();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // --- NeoPixel init ---
    pixel.begin();
    pixel.setBrightness(200);
    setColor(0, 0, 80);   // Blue = booting

    // --- Matrix init ---
    matrix.begin();
    matrix.setIntensity(5);
    showStatic("...");

    // --- Load saved city ---
    preferences.begin("redalert", true);
    String saved     = preferences.getString("city", "");
    bool forcePortal = preferences.getBool("portal", false);
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

    if (forcePortal) {
        preferences.begin("redalert", false);
        preferences.putBool("portal", false);
        preferences.end();
        Serial.println("[Setup] Portal requested by button");
    }

    showStatic(forcePortal ? "CONFIG" : "WiFi");
    wm.setConfigPortalTimeout(180);

    bool connected = forcePortal
        ? wm.startConfigPortal("RedAlert-Setup")
        : wm.autoConnect("RedAlert-Setup");

    if (!connected) {
        Serial.println("[Setup] Portal timeout — restarting");
        ESP.restart();
    }

    Serial.printf("[Setup] WiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());

    setColor(0, 80, 0);
    showStatic("OK!");
    delay(1500);

    applyState(SAFE);

    // Start alert task on core 0
    xTaskCreatePinnedToCore(alertTask, "alertTask", 8192, nullptr, 1, nullptr, 0);
}

// ===================== LOOP (core 1) =====================
AlertState    lastDisplayState = SAFE;
unsigned long btnPressTime     = 0;
bool          btnWasPressed    = false;

void loop() {
    // --- Long press: trigger config portal ---
    bool btnPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (btnPressed && !btnWasPressed) {
        btnPressTime  = millis();
        btnWasPressed = true;
    } else if (!btnPressed) {
        btnWasPressed = false;
    } else if (btnPressed && millis() - btnPressTime >= LONG_PRESS_MS) {
        Serial.println("[Button] Long press -> portal on next boot");
        preferences.begin("redalert", false);
        preferences.putBool("portal", true);
        preferences.end();
        ESP.restart();
    }

    // --- Compute display state ---
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    AlertState alertSt = currentState;
    xSemaphoreGive(stateMutex);

    AlertState displaySt;
    if (alertSt == SAFE) {
        if (apiFailCount >= API_FAIL_MAX) displaySt = NO_API;
        else if (!cityValid)             displaySt = BAD_CITY;
        else                             displaySt = SAFE;
    } else {
        displaySt = alertSt;   // alert states always take priority
    }

    // --- Apply when display changes ---
    if (displaySt != lastDisplayState) {
        lastDisplayState = displaySt;
        applyState(displaySt);
    }

    // --- Matrix animation ---
    bool scrolling = (displaySt == PRE_ALARM || displaySt == NO_API || displaySt == BAD_CITY);
    if (matrix.displayAnimate() && scrolling) {
        matrix.displayReset();
    }

    // --- LED effects ---
    // Fast flicker: ALARM
    if (displaySt == ALARM && millis() - lastBlink >= 60) {
        lastBlink = millis();
        blinkOn = !blinkOn;
        blinkOn ? setColor(80, 0, 0) : setColor(0, 0, 0);
    }
    // Slow blink: NO_API
    if (displaySt == NO_API && millis() - lastBlink >= 600) {
        lastBlink = millis();
        blinkOn = !blinkOn;
        blinkOn ? setColor(0, 0, 80) : setColor(0, 0, 0);
    }
}

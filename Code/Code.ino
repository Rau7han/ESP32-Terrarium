// ╔══════════════════════════════════════════════════════════════╗
// ║          SMART TERRARIUM v4.0 — ESP32                       ║
// ║  Features: DHT11 · Soil · OLED · NeoPixel · 3x Relay       ║
// ║  WiFi Dashboard · Auto/Manual · Scheduled Misting    ║
// ╚══════════════════════════════════════════════════════════════╝

// ── Libraries ──────────────────────────────────────────────────
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>

// ── OLED ───────────────────────────────────────────────────────
#define I2C_SDA      5
#define I2C_SCL      4
#define OLED_W     128
#define OLED_H      64
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ── DHT ────────────────────────────────────────────────────────
#define DHTPIN   2
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── Pins ───────────────────────────────────────────────────────
#define SOIL_PIN    36   // ADC1_CH0 — input-only
#define BUTTON_PIN  14   // INPUT_PULLUP
#define RELAY_MIST  25   // Mist Maker
#define RELAY_FAN   27   // Ventilation Fan
#define RELAY_PUMP  26   // Water Pump (NEW)
#define NEO_PIN     15   // NeoPixel data
#define NEO_COUNT   12

// ── Soil ADC calibration ───────────────────────────────────────
#define SOIL_DRY   4095
#define SOIL_WET   1800

// ── NeoPixel ───────────────────────────────────────────────────
Adafruit_NeoPixel ring(NEO_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);

// ── WiFi / NTP ─────────────────────────────────────────────────
const char* WIFI_SSID   = "ESP";
const char* WIFI_PASS   = "abcd1234";
const char* NTP_SERVER  = "pool.ntp.org";
const long  GMT_OFFSET  = 19800;   // IST = UTC+5:30
const int   DST_OFFSET  = 0;

WebServer   server(80);
Preferences prefs;
bool        ntpSynced = false;

// ── Timing constants ───────────────────────────────────────────
// One reading per minute stored in history; live display refreshes every 2s
const unsigned long SENSOR_INTERVAL  = 60000UL;  // 60 s — history samples
const unsigned long LIVE_INTERVAL    = 2000UL;   // 2 s  — live readings (OLED/LED)
const unsigned long DISPLAY_INTERVAL = 500UL;    // 0.5 s — OLED refresh

// Relay anti-flicker: minimum ms between toggling the same relay
const unsigned long RELAY_MIN_INTERVAL = 5000UL; // 5 s

// ── Hysteresis ─────────────────────────────────────────────────
// Applied on top of threshold values so relays don't rapid-toggle
// Fan ON  if temp > tempMax,  OFF if temp < tempMax - TEMP_HYST
// Mist ON if hum  < humMin,   OFF if hum  > humMin  + HUM_HYST
// Pump ON if soil < soilMin,  OFF if soil > soilMin + SOIL_HYST
#define TEMP_HYST   1.0f   // °C
#define HUM_HYST    5.0f   // %
#define SOIL_HYST   5      // %

// ── Sensor data ────────────────────────────────────────────────
struct Reading {
    float    temp;
    float    hum;
    int      soil;
    uint32_t epoch;  // Unix timestamp (or seconds since boot)
};

#define HIST_SIZE 100
Reading  history[HIST_SIZE];
int      histHead  = 0;
int      histCount = 0;

struct Live {
    float temp  = 0;
    float hum   = 0;
    int   soil  = 0;
    bool  dhtOk = false;
};
Live live;

// ── Relay state ────────────────────────────────────────────────
struct Relays {
    bool mist = false;
    bool fan  = false;
    bool pump = false;
};
Relays relays;

// Anti-flicker: last time each relay was toggled
unsigned long lastToggleMist = 0;
unsigned long lastToggleFan  = 0;
unsigned long lastTogglePump = 0;

// ── Mode ───────────────────────────────────────────────────────
bool autoMode = false;

// ── Auto thresholds (persistent) ──────────────────────────────
struct Thresholds {
    float tempMax  = 30.0f;
    float humMin   = 50.0f;
    float humMax   = 80.0f;
    int   soilMin  = 30;
    int   soilMax  = 70;
};
Thresholds thresh;

// ── Scheduled misting events ───────────────────────────────────
#define MAX_EVENTS 10
struct MistEvent {
    int  hour;
    int  minute;
    int  durationMin;
    bool enabled;
};
MistEvent mistEvents[MAX_EVENTS] = {
    { 6, 30, 5, true },
    { 9, 25, 5, true },
    {12, 40, 5, true },
    {14,  0, 5, true },
};
int           eventCount = 4;
unsigned long mistStopAt = 0;

// ── Button debounce ────────────────────────────────────────────
const unsigned long DEBOUNCE_MS   = 50;
const unsigned long LONG_PRESS_MS = 2000;

bool          btnRaw      = false;
bool          btnStable   = false;
bool          btnDown     = false;
unsigned long btnDownAt   = 0;
unsigned long btnDebounce = 0;

// ── Timing trackers ────────────────────────────────────────────
unsigned long lastSensor  = 0;   // history write
unsigned long lastLive    = 0;   // live read (OLED / LED)
unsigned long lastDisplay = 0;   // OLED repaint

// ── OLED animation ────────────────────────────────────────────
int oledPage = 0;   // 0=overview, 1=relays, cycles every 4 s

// ── Forward declarations ───────────────────────────────────────
void connectWiFi();
void syncNTP();
String timestamp();
uint32_t epochNow();
void loadPrefs();
void savePrefs();
void setupRoutes();
void sendJSON(int code, const String& j);
void readSensors();
void runAutoControl();
void checkScheduledMisting();
void setRelay(bool& state, int pin, bool on, const char* name, unsigned long& lastToggle);
void handleButton();
void updateOLED();
void updateNeoPixel();
void printBanner();

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(300);
    printBanner();

    // Persistent storage
    prefs.begin("terrarium", false);
    loadPrefs();

    // I2C + OLED
    Wire.begin(I2C_SDA, I2C_SCL);
    delay(80);
    bool oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (!oledOk) Serial.println("[OLED] Init failed");
    display.setTextColor(WHITE);
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,  0); display.println("SMART TERRARIUM v4");
    display.setCursor(0, 12); display.println("Initializing...");
    display.display();

    // Sensors
    dht.begin();
    analogSetPinAttenuation(SOIL_PIN, ADC_11db);

    // NeoPixel
    ring.begin(); ring.clear(); ring.show();

    // Button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Relays — ALL OFF at start (fail-safe)
    pinMode(RELAY_MIST, OUTPUT); digitalWrite(RELAY_MIST, LOW);
    pinMode(RELAY_FAN,  OUTPUT); digitalWrite(RELAY_FAN,  LOW);
    pinMode(RELAY_PUMP, OUTPUT); digitalWrite(RELAY_PUMP, LOW);

    // WiFi
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0,  0); display.println("WiFi Connected!");
        display.setCursor(0, 12); display.println("IP:");
        display.setTextSize(1);
        display.setCursor(0, 22); display.println(WiFi.localIP().toString());
        display.display();
        delay(2000);
    }

    syncNTP();

    // OTA
    ArduinoOTA.setHostname("terrarium-esp32");
    ArduinoOTA.onStart([]() {
        relays.mist = relays.fan = relays.pump = false;
        digitalWrite(RELAY_MIST, LOW);
        digitalWrite(RELAY_FAN,  LOW);
        digitalWrite(RELAY_PUMP, LOW);
        Serial.println("[OTA] Starting...");
    });
    ArduinoOTA.begin();

    setupRoutes();
    server.begin();

    // Do one immediate live read so OLED isn't blank
    readSensors(true);

    Serial.println("[SETUP] Complete");
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════
void loop() {
    ArduinoOTA.handle();
    server.handleClient();
    handleButton();

    unsigned long now = millis();

    // ── Live read every 2 s (for OLED / NeoPixel / auto-control) ──
    if (now - lastLive >= LIVE_INTERVAL) {
        readSensors(false);   // false = don't push to history
        if (autoMode)  runAutoControl();
        if (ntpSynced) checkScheduledMisting();

        // Stop scheduled mist when timer expires
        if (mistStopAt && now >= mistStopAt) {
            setRelay(relays.mist, RELAY_MIST, false, "Mist(sched-stop)", lastToggleMist);
            mistStopAt = 0;
        }
        lastLive = now;
    }

    // ── History write every 60 s ───────────────────────────────
    if (now - lastSensor >= SENSOR_INTERVAL) {
        // Push current live reading into history ring buffer
        Reading r;
        r.temp  = live.temp;
        r.hum   = live.hum;
        r.soil  = live.soil;
        r.epoch = epochNow();
        history[histHead] = r;
        histHead = (histHead + 1) % HIST_SIZE;
        if (histCount < HIST_SIZE) histCount++;
        Serial.printf("[History] Stored %.1f°C  %.0f%%  Soil:%d%%\n", live.temp, live.hum, live.soil);
        lastSensor = now;
    }

    // ── OLED + LED refresh ────────────────────────────────────
    if (now - lastDisplay >= DISPLAY_INTERVAL) {
        updateOLED();
        updateNeoPixel();
        lastDisplay = now;
    }

    // ── Non-blocking WiFi reconnect ───────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastRetry = 0;
        if (now - lastRetry > 10000) {
            lastRetry = now;
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        }
    }

    delay(5);
}

// ════════════════════════════════════════════════════════════════
//  WIFI & NTP
// ════════════════════════════════════════════════════════════════
void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
        delay(300); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("\n[WiFi] Failed — offline mode");
}

void syncNTP() {
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    struct tm t;
    if (getLocalTime(&t, 5000)) {
        ntpSynced = true;
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        Serial.printf("[NTP] %s\n", buf);
    } else {
        Serial.println("[NTP] Sync failed");
    }
}

String timestamp() {
    if (ntpSynced) {
        struct tm t;
        if (getLocalTime(&t)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
            return String(buf);
        }
    }
    return String(millis());
}

uint32_t epochNow() {
    if (ntpSynced) {
        struct tm t;
        if (getLocalTime(&t)) return (uint32_t)mktime(&t);
    }
    return (uint32_t)(millis() / 1000);
}

// ════════════════════════════════════════════════════════════════
//  PERSISTENT STORAGE
// ════════════════════════════════════════════════════════════════
void loadPrefs() {
    thresh.tempMax = prefs.getFloat("tempMax", 30.0f);
    thresh.humMin  = prefs.getFloat("humMin",  50.0f);
    thresh.humMax  = prefs.getFloat("humMax",  80.0f);
    thresh.soilMin = prefs.getInt  ("soilMin", 30);
    thresh.soilMax = prefs.getInt  ("soilMax", 70);
    autoMode       = prefs.getBool ("autoMode", false);
    Serial.printf("[Prefs] Loaded: tempMax=%.1f humMin=%.0f humMax=%.0f soilMin=%d soilMax=%d auto=%d\n",
                  thresh.tempMax, thresh.humMin, thresh.humMax,
                  thresh.soilMin, thresh.soilMax, autoMode);
}

void savePrefs() {
    prefs.putFloat("tempMax", thresh.tempMax);
    prefs.putFloat("humMin",  thresh.humMin);
    prefs.putFloat("humMax",  thresh.humMax);
    prefs.putInt  ("soilMin", thresh.soilMin);
    prefs.putInt  ("soilMax", thresh.soilMax);
    prefs.putBool ("autoMode", autoMode);
    Serial.println("[Prefs] Saved");
}

// ════════════════════════════════════════════════════════════════
//  SENSOR READING
//  pushHistory=true  → write into ring buffer (every 60 s)
//  pushHistory=false → update live struct only (every 2 s)
// ════════════════════════════════════════════════════════════════
void readSensors(bool pushHistory) {
    // ── DHT ─────────────────────────────────────────────────
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool  ok = (!isnan(t) && !isnan(h));

    // Fail-safe: if DHT fails and autoMode is on, turn off ALL relays
    if (!ok && autoMode) {
        if (live.dhtOk) {   // only act on transition to error
            Serial.println("[FAILSAFE] DHT error — all relays OFF");
            setRelay(relays.mist, RELAY_MIST, false, "FS-mist", lastToggleMist);
            setRelay(relays.fan,  RELAY_FAN,  false, "FS-fan",  lastToggleFan);
            setRelay(relays.pump, RELAY_PUMP, false, "FS-pump", lastTogglePump);
        }
    }
    live.dhtOk = ok;
    if (ok) { live.temp = t; live.hum = h; }
    else     Serial.println("[DHT] Read failed");

    // ── Soil ADC ────────────────────────────────────────────
    int raw  = analogRead(SOIL_PIN);
    live.soil = constrain(map(raw, SOIL_DRY, SOIL_WET, 0, 100), 0, 100);

    if (ok)
        Serial.printf("[Sensor] %.1f°C  %.0f%%  Soil:%d%%\n",
                      live.temp, live.hum, live.soil);
}

// ════════════════════════════════════════════════════════════════
//  AUTO CONTROL  (hysteresis + anti-flicker)
// ════════════════════════════════════════════════════════════════
void runAutoControl() {
    if (!live.dhtOk) return;   // fail-safe: never actuate on bad data
    unsigned long now = millis();

    // ── Mist (humidity) ─────────────────────────────────────
    if (!relays.mist && live.hum < thresh.humMin) {
        if (now - lastToggleMist >= RELAY_MIN_INTERVAL)
            setRelay(relays.mist, RELAY_MIST, true, "Mist(auto-on)", lastToggleMist);
    } else if (relays.mist && live.hum >= (thresh.humMin + HUM_HYST)) {
        if (now - lastToggleMist >= RELAY_MIN_INTERVAL)
            setRelay(relays.mist, RELAY_MIST, false, "Mist(auto-off)", lastToggleMist);
    }

    // ── Fan (temperature) ────────────────────────────────────
    if (!relays.fan && live.temp > thresh.tempMax) {
        if (now - lastToggleFan >= RELAY_MIN_INTERVAL)
            setRelay(relays.fan, RELAY_FAN, true, "Fan(auto-on)", lastToggleFan);
    } else if (relays.fan && live.temp <= (thresh.tempMax - TEMP_HYST)) {
        if (now - lastToggleFan >= RELAY_MIN_INTERVAL)
            setRelay(relays.fan, RELAY_FAN, false, "Fan(auto-off)", lastToggleFan);
    }

    // ── Pump (soil moisture) ─────────────────────────────────
    if (!relays.pump && live.soil < thresh.soilMin) {
        if (now - lastTogglePump >= RELAY_MIN_INTERVAL)
            setRelay(relays.pump, RELAY_PUMP, true, "Pump(auto-on)", lastTogglePump);
    } else if (relays.pump && live.soil >= (thresh.soilMin + SOIL_HYST)) {
        if (now - lastTogglePump >= RELAY_MIN_INTERVAL)
            setRelay(relays.pump, RELAY_PUMP, false, "Pump(auto-off)", lastTogglePump);
    }
}

// ════════════════════════════════════════════════════════════════
//  SCHEDULED MISTING
// ════════════════════════════════════════════════════════════════
void checkScheduledMisting() {
    struct tm t;
    if (!getLocalTime(&t)) return;
    int h = t.tm_hour, m = t.tm_min, s = t.tm_sec;
    if (s != 0) return;   // only fire at exact minute boundary

    for (int i = 0; i < eventCount; i++) {
        if (!mistEvents[i].enabled) continue;
        if (mistEvents[i].hour == h && mistEvents[i].minute == m) {
            setRelay(relays.mist, RELAY_MIST, true, "Mist(sched)", lastToggleMist);
            mistStopAt = millis() + (unsigned long)mistEvents[i].durationMin * 60000UL;
            Serial.printf("[Sched] Mist ON for %d min\n", mistEvents[i].durationMin);
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  RELAY HELPER  (with anti-flicker guard)
// ════════════════════════════════════════════════════════════════
void setRelay(bool& state, int pin, bool on, const char* name, unsigned long& lastToggle) {
    // Anti-flicker: skip if toggled too recently (unless turning off)
    // We always allow turning OFF (safety), but guard turning ON
    unsigned long now = millis();
    if (on && (now - lastToggle < RELAY_MIN_INTERVAL)) {
        Serial.printf("[Relay] %s ON skipped (anti-flicker)\n", name);
        return;
    }
    state = on;
    digitalWrite(pin, on ? HIGH : LOW);
    lastToggle = now;
    Serial.printf("[Relay] %s %s\n", name, on ? "ON" : "OFF");
}

// ════════════════════════════════════════════════════════════════
//  BUTTON — GPIO 14, INPUT_PULLUP
//  Short press  : toggle Auto / Manual mode
//  Long press   : EMERGENCY stop (all relays OFF)
// ════════════════════════════════════════════════════════════════
void handleButton() {
    bool reading = (digitalRead(BUTTON_PIN) == LOW);
    unsigned long now = millis();

    if (reading != btnRaw) { btnDebounce = now; btnRaw = reading; }
    if (now - btnDebounce < DEBOUNCE_MS) return;

    bool prev = btnStable;
    btnStable = reading;

    if (btnStable && !prev) {
        btnDown   = true;
        btnDownAt = now;
    }

    if (!btnStable && prev && btnDown) {
        btnDown = false;
        unsigned long dur = now - btnDownAt;

        if (dur >= LONG_PRESS_MS) {
            setRelay(relays.mist, RELAY_MIST, false, "EMERGENCY", lastToggleMist);
            setRelay(relays.fan,  RELAY_FAN,  false, "EMERGENCY", lastToggleFan);
            setRelay(relays.pump, RELAY_PUMP, false, "EMERGENCY", lastTogglePump);
            mistStopAt = 0;
            Serial.println("[Button] EMERGENCY STOP");
        } else {
            autoMode = !autoMode;
            savePrefs();
            Serial.printf("[Button] Mode → %s\n", autoMode ? "AUTO" : "MANUAL");
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  OLED — alternates between two pages every 4 s
//  Page 0: Temp / Hum / Soil / Mode
//  Page 1: Relay states (Mist / Fan / Pump) + WiFi
// ════════════════════════════════════════════════════════════════
void updateOLED() {
    static unsigned long lastPageSwitch = 0;
    if (millis() - lastPageSwitch > 4000) {
        oledPage = (oledPage + 1) % 2;
        lastPageSwitch = millis();
    }

    display.clearDisplay();
    display.setTextSize(1);

    // ── Title bar ──────────────────────────────────────────
    display.setCursor(0, 0);
    display.print("TERRARIUM ");
    display.print(autoMode ? "[AUTO]" : "[MANU]");
    display.drawLine(0, 9, 127, 9, WHITE);

    if (oledPage == 0) {
        // ── Sensors ───────────────────────────────────────
        if (!live.dhtOk) {
            display.setCursor(0, 14); display.println("DHT Error!");
        } else {
            display.setCursor(0, 14);
            display.print("Temp: "); display.print(live.temp, 1); display.print(" C");
            display.setCursor(0, 24);
            display.print("Hum:  "); display.print((int)live.hum); display.print(" %");
        }
        display.setCursor(0, 34);
        display.print("Soil: "); display.print(live.soil); display.print(" %");

        // Progress bar for soil
        int barW = map(live.soil, 0, 100, 0, 80);
        display.drawRect(44, 35, 82, 6, WHITE);
        if (barW > 0) display.fillRect(44, 35, barW, 6, WHITE);

    } else {
        // ── Relay status ──────────────────────────────────
        display.setCursor(0, 14);
        display.print("Mist:  "); display.println(relays.mist ? "ON " : "OFF");
        display.setCursor(0, 24);
        display.print("Fan:   "); display.println(relays.fan  ? "ON " : "OFF");
        display.setCursor(0, 34);
        display.print("Pump:  "); display.println(relays.pump ? "ON " : "OFF");
    }

    // ── Bottom status bar ─────────────────────────────────
    display.drawLine(0, 44, 127, 44, WHITE);
    display.setCursor(0, 48);
    display.print(WiFi.status() == WL_CONNECTED ? "WiFi:OK " : "WiFi:-- ");
    display.print(relays.mist ? "M" : "-");
    display.print(relays.fan  ? "F" : "-");
    display.print(relays.pump ? "P" : "-");

    // Page indicator dots
    display.fillCircle(oledPage == 0 ? 118 : 124, 52, 2, WHITE);
    display.drawCircle(oledPage == 0 ? 124 : 118, 52, 2, WHITE);

    display.display();
}

// ════════════════════════════════════════════════════════════════
//  NEOPIXEL  (spec LED colours)
//  Green  = soil OK (30–80%)
//  Blue   = humidity low / mist running
//  Purple = system nominal
//  Red    = sensor error / critical
//  Orange = fan running
//  Cyan   = pump running
// ════════════════════════════════════════════════════════════════
void updateNeoPixel() {
    ring.clear();
    bool assigned[NEO_COUNT] = {};

    // Relay indicator pixels: fixed positions
    if (relays.mist) {
        ring.setPixelColor(0, ring.Color(40, 40, 255));   // Blue
        ring.setPixelColor(1, ring.Color(20, 20, 160));
        assigned[0] = assigned[1] = true;
    }
    if (relays.fan) {
        ring.setPixelColor(4, ring.Color(255, 120, 0));   // Orange
        ring.setPixelColor(5, ring.Color(180, 80,  0));
        assigned[4] = assigned[5] = true;
    }
    if (relays.pump) {
        ring.setPixelColor(8,  ring.Color(0, 220, 220));  // Cyan
        ring.setPixelColor(9,  ring.Color(0, 140, 140));
        assigned[8] = assigned[9] = true;
    }

    // Ambient: remaining pixels
    uint32_t ambient;
    if (!live.dhtOk)       ambient = ring.Color(80, 0, 0);      // Red = error
    else if (live.soil < thresh.soilMin)
                           ambient = ring.Color(0, 0, 60);      // Blue = dry
    else if (live.soil > thresh.soilMax)
                           ambient = ring.Color(0, 60, 0);      // Green = wet
    else if (live.temp > thresh.tempMax)
                           ambient = ring.Color(80, 20, 0);     // Orange = hot
    else                   ambient = ring.Color(20, 0, 40);     // Purple = OK

    for (int i = 0; i < NEO_COUNT; i++)
        if (!assigned[i]) ring.setPixelColor(i, ambient);

    ring.show();
}

// ════════════════════════════════════════════════════════════════
//  API HELPERS
// ════════════════════════════════════════════════════════════════
void sendJSON(int code, const String& j) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(code, "application/json", j);
}

// ════════════════════════════════════════════════════════════════
//  API HANDLERS
// ════════════════════════════════════════════════════════════════

// GET /api/live
void apiLive() {
    StaticJsonDocument<320> doc;
    doc["temp"]     = live.temp;
    doc["hum"]      = live.hum;
    doc["soil"]     = live.soil;
    doc["dhtOk"]    = live.dhtOk;
    doc["mist"]     = relays.mist;
    doc["fan"]      = relays.fan;
    doc["pump"]     = relays.pump;
    doc["autoMode"] = autoMode;
    doc["uptime"]   = millis() / 1000;
    doc["ts"]       = timestamp();
    doc["wifi"]     = (WiFi.status() == WL_CONNECTED);
    String j; serializeJson(doc, j);
    sendJSON(200, j);
}

// GET /api/history  — returns last N readings (1 per minute)
void apiHistory() {
    int n = min(histCount, HIST_SIZE);
    DynamicJsonDocument doc(10240);
    JsonArray arrTemp  = doc.createNestedArray("temp");
    JsonArray arrHum   = doc.createNestedArray("hum");
    JsonArray arrSoil  = doc.createNestedArray("soil");
    JsonArray arrEpoch = doc.createNestedArray("epoch");

    int start = (histHead - n + HIST_SIZE) % HIST_SIZE;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % HIST_SIZE;
        arrTemp.add( serialized(String(history[idx].temp, 1)));
        arrHum.add(  serialized(String(history[idx].hum,  0)));
        arrSoil.add( history[idx].soil);
        arrEpoch.add(history[idx].epoch);
    }

    String j; serializeJson(doc, j);
    sendJSON(200, j);
}

// GET /api/thresholds
void apiGetThresholds() {
    StaticJsonDocument<160> doc;
    doc["tempMax"]  = thresh.tempMax;
    doc["humMin"]   = thresh.humMin;
    doc["humMax"]   = thresh.humMax;
    doc["soilMin"]  = thresh.soilMin;
    doc["soilMax"]  = thresh.soilMax;
    String j; serializeJson(doc, j);
    sendJSON(200, j);
}

// POST /api/thresholds
void apiSetThresholds() {
    if (!server.hasArg("plain")) { sendJSON(400, "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<160> doc;
    if (deserializeJson(doc, server.arg("plain"))) { sendJSON(400, "{\"error\":\"bad json\"}"); return; }
    if (doc["tempMax"].is<float>()) thresh.tempMax = doc["tempMax"];
    if (doc["humMin"].is<float>())  thresh.humMin  = doc["humMin"];
    if (doc["humMax"].is<float>())  thresh.humMax  = doc["humMax"];
    if (doc["soilMin"].is<int>())   thresh.soilMin = doc["soilMin"];
    if (doc["soilMax"].is<int>())   thresh.soilMax = doc["soilMax"];
    savePrefs();
    sendJSON(200, "{\"ok\":true}");
}

// POST /api/relay  {"relay":"mist"|"fan"|"pump", "state":true|false}
void apiRelay() {
    if (autoMode) { sendJSON(403, "{\"error\":\"switch to manual first\"}"); return; }
    if (!server.hasArg("plain")) { sendJSON(400, "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<80> doc;
    if (deserializeJson(doc, server.arg("plain"))) { sendJSON(400, "{\"error\":\"bad json\"}"); return; }
    const char* which = doc["relay"] | "";
    bool on = doc["state"] | false;
    if      (strcmp(which, "mist") == 0) setRelay(relays.mist, RELAY_MIST, on, "Mist(web)", lastToggleMist);
    else if (strcmp(which, "fan")  == 0) setRelay(relays.fan,  RELAY_FAN,  on, "Fan(web)",  lastToggleFan);
    else if (strcmp(which, "pump") == 0) setRelay(relays.pump, RELAY_PUMP, on, "Pump(web)", lastTogglePump);
    else { sendJSON(400, "{\"error\":\"unknown relay\"}"); return; }
    sendJSON(200, "{\"ok\":true}");
}

// POST /api/mode  {"auto":true|false}
void apiMode() {
    if (!server.hasArg("plain")) { sendJSON(400, "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<32> doc;
    if (deserializeJson(doc, server.arg("plain"))) { sendJSON(400, "{\"error\":\"bad json\"}"); return; }
    autoMode = doc["auto"] | autoMode;
    savePrefs();
    sendJSON(200, "{\"ok\":true}");
}

// GET /api/events
void apiGetEvents() {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < eventCount; i++) {
        JsonObject e = arr.createNestedObject();
        e["id"]      = i;
        e["hour"]    = mistEvents[i].hour;
        e["minute"]  = mistEvents[i].minute;
        e["dur"]     = mistEvents[i].durationMin;
        e["enabled"] = mistEvents[i].enabled;
    }
    String j; serializeJson(doc, j);
    sendJSON(200, j);
}

// POST /api/events
void apiSetEvent() {
    if (!server.hasArg("plain")) { sendJSON(400, "{\"error\":\"no body\"}"); return; }
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) { sendJSON(400, "{\"error\":\"bad json\"}"); return; }
    int id = doc["id"] | -1;
    if (id < 0 || id >= MAX_EVENTS) { sendJSON(400, "{\"error\":\"bad id\"}"); return; }
    mistEvents[id].hour        = doc["hour"]    | mistEvents[id].hour;
    mistEvents[id].minute      = doc["minute"]  | mistEvents[id].minute;
    mistEvents[id].durationMin = doc["dur"]     | mistEvents[id].durationMin;
    mistEvents[id].enabled     = doc["enabled"] | mistEvents[id].enabled;
    if (id >= eventCount) eventCount = id + 1;
    sendJSON(200, "{\"ok\":true}");
}

// ════════════════════════════════════════════════════════════════
//  WEB SERVER ROUTES
// ════════════════════════════════════════════════════════════════
void setupRoutes() {
    // CORS preflight
    auto cors = []() { sendJSON(204, ""); };
    server.on("/api/live",       HTTP_OPTIONS, cors);
    server.on("/api/history",    HTTP_OPTIONS, cors);
    server.on("/api/thresholds", HTTP_OPTIONS, cors);
    server.on("/api/relay",      HTTP_OPTIONS, cors);
    server.on("/api/mode",       HTTP_OPTIONS, cors);
    server.on("/api/events",     HTTP_OPTIONS, cors);

    server.on("/api/live",       HTTP_GET,  apiLive);
    server.on("/api/history",    HTTP_GET,  apiHistory);
    server.on("/api/thresholds", HTTP_GET,  apiGetThresholds);
    server.on("/api/thresholds", HTTP_POST, apiSetThresholds);
    server.on("/api/relay",      HTTP_POST, apiRelay);
    server.on("/api/mode",       HTTP_POST, apiMode);
    server.on("/api/events",     HTTP_GET,  apiGetEvents);
    server.on("/api/events",     HTTP_POST, apiSetEvent);

    server.on("/", HTTP_GET, handleRoot);
    server.onNotFound([]() { sendJSON(404, "{\"error\":\"not found\"}"); });
}

// ════════════════════════════════════════════════════════════════
//  HTML DASHBOARD  — Premium Terrarium UI v4
// ════════════════════════════════════════════════════════════════
void handleRoot() {
    String html = R"RAWHTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Terrarium</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Space+Mono:ital,wght@0,400;0,700;1,400&family=DM+Sans:opsz,wght@9..40,300;9..40,400;9..40,500;9..40,700&display=swap" rel="stylesheet">
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
/* ═══ RESET & VARS ═══════════════════════════════════════════ */
:root {
  --bg:       #0d1117;
  --surface:  #161b22;
  --surface2: #1c2128;
  --border:   #30363d;
  --border2:  #21262d;
  --text:     #e6edf3;
  --muted:    #7d8590;
  --muted2:   #484f58;
  --green:    #3fb950;
  --green-bg: rgba(63,185,80,.1);
  --blue:     #58a6ff;
  --blue-bg:  rgba(88,166,255,.1);
  --cyan:     #39d3f2;
  --cyan-bg:  rgba(57,211,242,.1);
  --orange:   #e3b341;
  --orange-bg:rgba(227,179,65,.1);
  --red:      #f85149;
  --red-bg:   rgba(248,81,73,.1);
  --purple:   #bc8cff;
  --purple-bg:rgba(188,140,255,.1);
  --radius:   10px;
  --mono: 'Space Mono', monospace;
  --sans: 'DM Sans', sans-serif;
}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:var(--sans);background:var(--bg);color:var(--text);display:flex;min-height:100vh;overflow:hidden}

/* ═══ SIDEBAR ════════════════════════════════════════════════ */
.sidebar {
  width:220px;min-width:220px;
  background:var(--surface);
  border-right:1px solid var(--border);
  display:flex;flex-direction:column;
  transition:width .25s;
  z-index:10;
}
.logo {
  padding:20px 20px 16px;
  border-bottom:1px solid var(--border2);
  display:flex;align-items:center;gap:12px;
}
.logo-icon {
  width:36px;height:36px;border-radius:10px;
  background:linear-gradient(135deg,var(--green),#1a7f3c);
  display:flex;align-items:center;justify-content:center;
  font-size:18px;flex-shrink:0;
  box-shadow:0 0 16px rgba(63,185,80,.3);
}
.logo-text { font-family:var(--mono); font-size:.78em; line-height:1.4; }
.logo-text strong { color:var(--text); font-size:1.05em; }
.logo-text small  { color:var(--muted); }

nav { flex:1; padding:12px 10px; overflow-y:auto; }
.nav-section { font-size:.65em; font-weight:700; letter-spacing:1.5px;
  text-transform:uppercase; color:var(--muted2); padding:14px 10px 6px; }
.nav-item {
  display:flex;align-items:center;gap:10px;
  padding:9px 12px;border-radius:8px;
  font-size:.85em;font-weight:500;color:var(--muted);
  cursor:pointer;transition:all .15s;margin-bottom:2px;
}
.nav-item:hover { background:var(--surface2); color:var(--text); }
.nav-item.active { background:var(--green-bg); color:var(--green); }
.nav-item svg { flex-shrink:0;width:16px;height:16px; }

.sidebar-footer {
  padding:14px 16px;border-top:1px solid var(--border2);
  font-size:.72em;color:var(--muted);font-family:var(--mono);
}
.conn-row { display:flex;align-items:center;gap:8px;margin-bottom:4px; }
.dot { width:7px;height:7px;border-radius:50%;background:var(--muted2);flex-shrink:0; }
.dot.on  { background:var(--green);box-shadow:0 0 6px var(--green);animation:pulse 2s infinite; }
.dot.err { background:var(--red); }
@keyframes pulse { 0%,100%{opacity:1}50%{opacity:.35} }

/* ═══ MAIN AREA ═══════════════════════════════════════════════ */
.main { flex:1;display:flex;flex-direction:column;min-width:0;overflow:hidden; }

.topbar {
  display:flex;align-items:center;justify-content:space-between;
  padding:14px 28px;
  background:var(--surface);
  border-bottom:1px solid var(--border);
  gap:16px;flex-shrink:0;
}
.topbar-left { display:flex;align-items:center;gap:14px; }
.page-title  { font-size:1.05em;font-weight:700;letter-spacing:.5px; }
.breadcrumb  { font-size:.75em;color:var(--muted);font-family:var(--mono); }

.topbar-right { display:flex;align-items:center;gap:10px; }

.mode-pill {
  display:flex;align-items:center;gap:7px;
  padding:6px 14px;border-radius:20px;
  font-size:.78em;font-weight:700;font-family:var(--mono);
  cursor:pointer;border:1px solid var(--border);
  background:var(--surface2);color:var(--muted);
  transition:all .2s;user-select:none;
}
.mode-pill.auto {
  border-color:var(--green);color:var(--green);
  background:var(--green-bg);
  box-shadow:0 0 12px rgba(63,185,80,.15);
}
.mode-pill .dot { width:6px;height:6px; }
.mode-pill.auto .dot { background:var(--green); }

.ts-badge {
  font-size:.72em;color:var(--muted);font-family:var(--mono);
  padding:5px 10px;border-radius:6px;
  background:var(--surface2);border:1px solid var(--border2);
}

/* ═══ PAGES ══════════════════════════════════════════════════ */
.content { flex:1;overflow-y:auto;padding:24px 28px; }
.page    { display:none; }
.page.active { display:block; }

/* ═══ KPI CARDS ══════════════════════════════════════════════ */
.kpi-grid {
  display:grid;
  grid-template-columns:repeat(auto-fill,minmax(180px,1fr));
  gap:14px;margin-bottom:22px;
}
.kpi {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:18px 18px 14px;
  position:relative;overflow:hidden;transition:border-color .2s;
}
.kpi:hover { border-color:var(--border); }
.kpi-accent {
  position:absolute;top:0;left:0;right:0;height:2px;
  border-radius:var(--radius) var(--radius) 0 0;
}
.kpi-label {
  font-size:.7em;font-weight:600;letter-spacing:1px;
  text-transform:uppercase;color:var(--muted);margin-bottom:10px;
  display:flex;align-items:center;gap:6px;
}
.kpi-icon { width:14px;height:14px;opacity:.6; }
.kpi-val  { font-size:2.2em;font-weight:700;font-family:var(--mono);line-height:1; }
.kpi-unit { font-size:.38em;font-weight:400;color:var(--muted);margin-left:3px; }
.kpi-footer {
  margin-top:10px;display:flex;align-items:center;justify-content:space-between;
}
.kpi-badge {
  font-size:.7em;font-weight:600;padding:2px 9px;border-radius:10px;
  background:var(--surface2);color:var(--muted);
}
.kpi-badge.ok     { background:var(--green-bg);color:var(--green); }
.kpi-badge.warn   { background:var(--orange-bg);color:var(--orange); }
.kpi-badge.err    { background:var(--red-bg);color:var(--red); }
.kpi-badge.active { background:var(--blue-bg);color:var(--blue); }
.kpi-badge.cyan   { background:var(--cyan-bg);color:var(--cyan); }
.kpi-trend { font-size:.72em;color:var(--muted);font-family:var(--mono); }

/* ═══ SECTION HEADERS ════════════════════════════════════════ */
.section-header {
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:14px;
}
.section-title { font-size:.85em;font-weight:700;color:var(--text); }
.section-sub   { font-size:.75em;color:var(--muted);margin-top:2px; }

/* ═══ CHART CARDS ════════════════════════════════════════════ */
.chart-card {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:20px;margin-bottom:18px;
}
.chart-header {
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:16px;
}
.chart-title { font-size:.85em;font-weight:700; }
.chart-tabs  { display:flex;gap:2px;background:var(--surface2);
  border-radius:8px;padding:3px; }
.tab {
  padding:5px 12px;border-radius:6px;font-size:.75em;font-weight:600;
  color:var(--muted);cursor:pointer;transition:all .15s;
}
.tab.active { background:var(--surface);color:var(--text);
  box-shadow:0 1px 4px rgba(0,0,0,.3); }
.chart-wrap { height:220px;position:relative; }
.chart-wrap-tall { height:280px;position:relative; }

/* ═══ ALERTS ═════════════════════════════════════════════════ */
.alert-strip {
  padding:10px 14px;border-radius:8px;margin-bottom:12px;
  font-size:.82em;display:flex;align-items:center;gap:10px;
  border:1px solid;animation:slideIn .3s ease;
}
@keyframes slideIn{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:none}}
.alert-strip.warn { border-color:var(--orange);background:var(--orange-bg);color:var(--orange); }
.alert-strip.err  { border-color:var(--red);background:var(--red-bg);color:var(--red); }
.alert-strip svg  { flex-shrink:0;width:15px;height:15px; }
.alerts-box       { margin-bottom:18px; }

/* ═══ GAUGE BARS (home page) ═════════════════════════════════ */
.gauge-grid { display:grid;grid-template-columns:1fr 1fr 1fr;gap:14px;margin-bottom:18px; }
.gauge-card {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:16px;
}
.gauge-label { font-size:.7em;color:var(--muted);font-weight:600;
  letter-spacing:.8px;text-transform:uppercase;margin-bottom:8px;
  display:flex;justify-content:space-between;align-items:center;
}
.gauge-val-sm { font-family:var(--mono);font-size:.9em;color:var(--text); }
.gauge-track {
  height:8px;background:var(--surface2);border-radius:4px;
  overflow:hidden;margin-bottom:6px;
}
.gauge-fill { height:100%;border-radius:4px;transition:width .6s ease; }
.gauge-limits { display:flex;justify-content:space-between;font-size:.65em;color:var(--muted2); }

/* ═══ RELAY CONTROLS ═════════════════════════════════════════ */
.relay-grid { display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:14px; }
.relay-card {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:20px;
  display:flex;flex-direction:column;gap:14px;transition:border-color .2s;
}
.relay-card.on { border-color:var(--green); }
.relay-card.on.blue { border-color:var(--blue); }
.relay-card.on.cyan { border-color:var(--cyan); }
.relay-card.on.orange { border-color:var(--orange); }
.relay-top { display:flex;align-items:center;justify-content:space-between; }
.relay-name { font-size:.88em;font-weight:700; }
.relay-pin  { font-size:.68em;color:var(--muted);font-family:var(--mono); }
.relay-icon { font-size:1.8em;margin-bottom:4px; }
.relay-state {
  font-size:.75em;font-weight:700;font-family:var(--mono);
  padding:4px 10px;border-radius:6px;
  background:var(--surface2);color:var(--muted);
}
.relay-state.on { background:var(--green-bg);color:var(--green); }

/* Toggle switch */
.toggle-wrap { display:flex;align-items:center;gap:10px;cursor:pointer; }
.toggle {
  position:relative;width:46px;height:24px;flex-shrink:0;
}
.toggle input { opacity:0;width:0;height:0; }
.track {
  position:absolute;inset:0;background:var(--surface2);
  border-radius:24px;border:1px solid var(--border);
  cursor:pointer;transition:.25s;
}
.track::before {
  content:"";position:absolute;height:18px;width:18px;
  left:2px;top:2px;background:var(--muted2);
  border-radius:50%;transition:.25s;
}
input:checked+.track { background:var(--green);border-color:var(--green); }
input:checked+.track::before { transform:translateX(22px);background:#fff; }
.toggle-label { font-size:.82em;color:var(--muted); }

/* ═══ THRESHOLD CONTROLS ════════════════════════════════════ */
.thresh-card {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:22px;
}
.thresh-item { margin-bottom:18px; }
.thresh-head {
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:8px;
}
.thresh-name { font-size:.8em;font-weight:600; }
.thresh-val  {
  font-family:var(--mono);font-size:.8em;
  color:var(--blue);font-weight:700;
  padding:2px 8px;border-radius:5px;background:var(--blue-bg);
  min-width:56px;text-align:center;
}
input[type=range] {
  -webkit-appearance:none;width:100%;height:5px;
  background:var(--surface2);border-radius:3px;outline:none;
  cursor:pointer;
}
input[type=range]::-webkit-slider-thumb {
  -webkit-appearance:none;width:16px;height:16px;border-radius:50%;
  background:var(--blue);cursor:pointer;
  box-shadow:0 0 6px rgba(88,166,255,.4);
  transition:box-shadow .15s;
}
input[type=range]:hover::-webkit-slider-thumb {
  box-shadow:0 0 12px rgba(88,166,255,.6);
}
.thresh-desc { font-size:.68em;color:var(--muted);margin-top:5px; }

.save-btn {
  display:flex;align-items:center;gap:8px;
  padding:10px 22px;
  background:linear-gradient(135deg,var(--green),#1e8a3e);
  color:#fff;border:none;border-radius:8px;
  font-size:.85em;font-weight:700;font-family:var(--sans);
  cursor:pointer;transition:all .2s;margin-top:20px;
}
.save-btn:hover { opacity:.9;transform:translateY(-1px); }
.save-btn:active { transform:translateY(0); }

/* ═══ EVENTS TABLE ═══════════════════════════════════════════ */
.ev-table-wrap {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);overflow:hidden;margin-bottom:18px;
}
.ev-table { width:100%;border-collapse:collapse; }
.ev-table th {
  padding:11px 16px;font-size:.7em;font-weight:700;
  text-transform:uppercase;letter-spacing:1px;
  color:var(--muted);text-align:left;
  background:var(--surface2);border-bottom:1px solid var(--border);
}
.ev-table td {
  padding:11px 16px;font-size:.83em;
  border-bottom:1px solid var(--border2);
}
.ev-table tr:last-child td { border-bottom:none; }
.ev-table tr:hover td { background:var(--surface2); }
.ev-chip {
  display:inline-flex;align-items:center;gap:5px;
  padding:3px 10px;border-radius:20px;font-size:.75em;font-weight:600;
  background:var(--blue-bg);color:var(--blue);
}
.time-mono { font-family:var(--mono);font-size:.9em; }

/* Timeline */
.timeline-card {
  background:var(--surface);border:1px solid var(--border);
  border-radius:var(--radius);padding:20px;
}
.tl-row   { display:flex;align-items:center;gap:12px;margin-bottom:10px; }
.tl-label { font-size:.72em;color:var(--muted);width:50px;flex-shrink:0;font-family:var(--mono); }
.tl-track { flex:1;height:20px;background:var(--surface2);border-radius:5px;position:relative;overflow:hidden; }
.tl-tick  {
  position:absolute;top:0;height:100%;border-radius:3px;
  background:linear-gradient(90deg,var(--blue),var(--cyan));opacity:.8;
}
.tl-labels {
  display:flex;justify-content:space-between;
  font-size:.62em;color:var(--muted2);margin-top:4px;font-family:var(--mono);
}

/* ═══ STATS ROW ══════════════════════════════════════════════ */
.stats-row {
  display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin-bottom:18px;
}
.stat-mini {
  background:var(--surface);border:1px solid var(--border);
  border-radius:8px;padding:14px;text-align:center;
}
.stat-mini .v { font-family:var(--mono);font-size:1.3em;font-weight:700;color:var(--text); }
.stat-mini .l { font-size:.65em;color:var(--muted);margin-top:4px;text-transform:uppercase;letter-spacing:.8px; }

/* ═══ TWO COL LAYOUT ════════════════════════════════════════ */
.two-col { display:grid;grid-template-columns:1fr 1fr;gap:18px; }

/* ═══ SCROLLBAR ══════════════════════════════════════════════ */
::-webkit-scrollbar { width:5px; }
::-webkit-scrollbar-track { background:transparent; }
::-webkit-scrollbar-thumb { background:var(--border);border-radius:3px; }

/* ═══ RESPONSIVE ═════════════════════════════════════════════ */
@media(max-width:900px){
  .two-col { grid-template-columns:1fr; }
  .gauge-grid { grid-template-columns:1fr 1fr; }
  .stats-row  { grid-template-columns:1fr 1fr 1fr; }
}
@media(max-width:640px){
  .sidebar    { width:52px;min-width:52px; }
  .logo-text,.nav-item span,.nav-section,.sidebar-footer { display:none; }
  .logo-icon  { margin:0 auto; }
  .nav-item   { justify-content:center;padding:12px 0; }
  .content    { padding:14px; }
  .topbar     { padding:12px 16px; }
  .gauge-grid { grid-template-columns:1fr; }
  .kpi-grid   { grid-template-columns:1fr 1fr; }
  .relay-grid { grid-template-columns:1fr; }
}
/* ═══ TOAST ═══════════════════════════════════════════════════ */
.toast {
  position:fixed;bottom:24px;right:24px;z-index:1000;
  padding:12px 20px;border-radius:9px;font-size:.82em;font-weight:600;
  background:var(--surface);border:1px solid var(--border);
  color:var(--text);box-shadow:0 8px 32px rgba(0,0,0,.4);
  transform:translateY(20px);opacity:0;transition:all .3s;pointer-events:none;
}
.toast.show { transform:translateY(0);opacity:1; }
.toast.ok   { border-color:var(--green);color:var(--green); }
.toast.err  { border-color:var(--red);color:var(--red); }
</style>
</head>
<body>

<!-- ══ SIDEBAR ══════════════════════════════════════════════ -->
<aside class="sidebar">
  <div class="logo">
    <div class="logo-icon">🦎</div>
    <div class="logo-text">
      <strong>TERRARIUM</strong><br>
      <small>v4.0 · ESP32</small>
    </div>
  </div>
  <nav>
    <div class="nav-section">Monitor</div>
    <div class="nav-item active" data-page="home" onclick="showPage(this,'home')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 9l9-7 9 7v11a2 2 0 01-2 2H5a2 2 0 01-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg>
      <span>Overview</span>
    </div>
    <div class="nav-item" data-page="measurements" onclick="showPage(this,'measurements')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
      <span>Measurements</span>
    </div>
    <div class="nav-section">Manage</div>
    <div class="nav-item" data-page="controls" onclick="showPage(this,'controls')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.07 4.93a10 10 0 010 14.14M4.93 4.93a10 10 0 000 14.14"/></svg>
      <span>Controls</span>
    </div>
    <div class="nav-item" data-page="thresholds" onclick="showPage(this,'thresholds')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/></svg>
      <span>Thresholds</span>
    </div>
    <div class="nav-item" data-page="events" onclick="showPage(this,'events')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="4" width="18" height="18" rx="2"/><line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/></svg>
      <span>Schedule</span>
    </div>
  </nav>
  <div class="sidebar-footer">
    <div class="conn-row">
      <span class="dot" id="connDot"></span>
      <span id="connLabel">Connecting…</span>
    </div>
    <div id="uptimeLabel" style="color:var(--muted2)">—</div>
  </div>
</aside>

<!-- ══ MAIN ══════════════════════════════════════════════════ -->
<div class="main">
  <div class="topbar">
    <div class="topbar-left">
      <div>
        <div class="page-title" id="pageTitle">Overview</div>
        <div class="breadcrumb">Smart Terrarium</div>
      </div>
    </div>
    <div class="topbar-right">
      <div class="ts-badge" id="tsBadge">--:--:--</div>
      <div class="mode-pill" id="modePill" onclick="toggleMode()">
        <span class="dot"></span>
        <span id="modeText">MANUAL</span>
      </div>
    </div>
  </div>

  <div class="content">

    <!-- ═══ OVERVIEW PAGE ════════════════════════════════════ -->
    <div class="page active" id="pg-home">
      <div class="alerts-box" id="alertsBox"></div>

      <!-- KPI row -->
      <div class="kpi-grid">
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,#ef5350,#ff7043)"></div>
          <div class="kpi-label">
            <svg class="kpi-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 14.76V3.5a2.5 2.5 0 00-5 0v11.26a4.5 4.5 0 105 0z"/></svg>
            Temperature
          </div>
          <div class="kpi-val"><span id="h-temp">--</span><span class="kpi-unit">°C</span></div>
          <div class="kpi-footer">
            <span class="kpi-badge" id="h-tempS">--</span>
            <span class="kpi-trend" id="h-tempTrend"></span>
          </div>
        </div>
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,#42a5f5,#7e57c2)"></div>
          <div class="kpi-label">
            <svg class="kpi-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2.69l5.66 5.66a8 8 0 11-11.31 0z"/></svg>
            Humidity
          </div>
          <div class="kpi-val"><span id="h-hum">--</span><span class="kpi-unit">%</span></div>
          <div class="kpi-footer"><span class="kpi-badge" id="h-humS">--</span></div>
        </div>
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,#66bb6a,#aed581)"></div>
          <div class="kpi-label">
            <svg class="kpi-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 22V12M12 12C12 6 6 2 6 2s1 5 6 10zM12 12c0-6 6-10 6-10s-1 5-6 10z"/></svg>
            Soil Moisture
          </div>
          <div class="kpi-val"><span id="h-soil">--</span><span class="kpi-unit">%</span></div>
          <div class="kpi-footer"><span class="kpi-badge" id="h-soilS">--</span></div>
        </div>
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,var(--blue),var(--cyan))"></div>
          <div class="kpi-label">💧 Mist Maker</div>
          <div class="kpi-val" style="font-size:1.35em" id="h-mist">--</div>
          <div class="kpi-footer"><span class="kpi-badge" id="h-mistS">--</span></div>
        </div>
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,var(--orange),#ff7043)"></div>
          <div class="kpi-label">🌀 Vent Fan</div>
          <div class="kpi-val" style="font-size:1.35em" id="h-fan">--</div>
          <div class="kpi-footer"><span class="kpi-badge" id="h-fanS">--</span></div>
        </div>
        <div class="kpi">
          <div class="kpi-accent" style="background:linear-gradient(90deg,var(--cyan),var(--green))"></div>
          <div class="kpi-label">🚿 Water Pump</div>
          <div class="kpi-val" style="font-size:1.35em" id="h-pump">--</div>
          <div class="kpi-footer"><span class="kpi-badge" id="h-pumpS">--</span></div>
        </div>
      </div>

      <!-- Gauge bars -->
      <div class="gauge-grid">
        <div class="gauge-card">
          <div class="gauge-label">Temperature <span class="gauge-val-sm" id="g-tempV">--°C</span></div>
          <div class="gauge-track"><div class="gauge-fill" id="g-tempF" style="background:linear-gradient(90deg,#42a5f5,#ef5350);width:0%"></div></div>
          <div class="gauge-limits"><span>15°C</span><span>45°C</span></div>
        </div>
        <div class="gauge-card">
          <div class="gauge-label">Humidity <span class="gauge-val-sm" id="g-humV">--%</span></div>
          <div class="gauge-track"><div class="gauge-fill" id="g-humF" style="background:linear-gradient(90deg,var(--orange),var(--blue));width:0%"></div></div>
          <div class="gauge-limits"><span>0%</span><span>100%</span></div>
        </div>
        <div class="gauge-card">
          <div class="gauge-label">Soil Moisture <span class="gauge-val-sm" id="g-soilV">--%</span></div>
          <div class="gauge-track"><div class="gauge-fill" id="g-soilF" style="background:linear-gradient(90deg,#ef5350,var(--green));width:0%"></div></div>
          <div class="gauge-limits"><span>Dry</span><span>Wet</span></div>
        </div>
      </div>

      <!-- Live chart -->
      <div class="chart-card">
        <div class="chart-header">
          <div class="chart-title">Live Readings (last 20)</div>
          <div style="font-size:.72em;color:var(--muted);font-family:var(--mono)">Updates every 2s</div>
        </div>
        <div class="chart-wrap-tall"><canvas id="homeChart"></canvas></div>
      </div>
    </div>

    <!-- ═══ MEASUREMENTS PAGE ════════════════════════════════ -->
    <div class="page" id="pg-measurements">
      <!-- Stats mini row -->
      <div class="stats-row" id="statsRow">
        <div class="stat-mini"><div class="v" id="st-min">--</div><div class="l">Min</div></div>
        <div class="stat-mini"><div class="v" id="st-avg">--</div><div class="l">Avg</div></div>
        <div class="stat-mini"><div class="v" id="st-max">--</div><div class="l">Max</div></div>
      </div>
      <div class="chart-card">
        <div class="chart-header">
          <div>
            <div class="chart-title" id="measTitle">Temperature History</div>
            <div style="font-size:.7em;color:var(--muted);margin-top:3px">1 reading / minute · up to 100 points</div>
          </div>
          <div class="chart-tabs">
            <div class="tab active" onclick="switchTab('temp',this)">Temp</div>
            <div class="tab" onclick="switchTab('hum',this)">Hum</div>
            <div class="tab" onclick="switchTab('soil',this)">Soil</div>
          </div>
        </div>
        <div class="chart-wrap-tall"><canvas id="measChart"></canvas></div>
      </div>
    </div>

    <!-- ═══ CONTROLS PAGE ════════════════════════════════════ -->
    <div class="page" id="pg-controls">
      <p style="font-size:.78em;color:var(--muted);margin-bottom:18px;padding:10px 14px;background:var(--surface);border:1px solid var(--border);border-radius:8px;">
        ⚡ Switch to <strong>MANUAL</strong> mode (top-right) to override relays manually. Auto mode controls relays via thresholds.
      </p>
      <div class="relay-grid" id="relayGrid">

        <div class="relay-card" id="rc-mist">
          <div class="relay-top">
            <div>
              <div class="relay-icon">💧</div>
              <div class="relay-name">Mist Maker</div>
              <div class="relay-pin">GPIO 25 · RELAY_MIST</div>
            </div>
            <span class="relay-state" id="rs-mist">OFF</span>
          </div>
          <label class="toggle-wrap">
            <div class="toggle">
              <input type="checkbox" id="sw-mist" onchange="setRelay('mist',this.checked)">
              <span class="track"></span>
            </div>
            <span class="toggle-label">Toggle Mist</span>
          </label>
        </div>

        <div class="relay-card" id="rc-fan">
          <div class="relay-top">
            <div>
              <div class="relay-icon">🌀</div>
              <div class="relay-name">Ventilation Fan</div>
              <div class="relay-pin">GPIO 16 · RELAY_FAN</div>
            </div>
            <span class="relay-state" id="rs-fan">OFF</span>
          </div>
          <label class="toggle-wrap">
            <div class="toggle">
              <input type="checkbox" id="sw-fan" onchange="setRelay('fan',this.checked)">
              <span class="track"></span>
            </div>
            <span class="toggle-label">Toggle Fan</span>
          </label>
        </div>

        <div class="relay-card" id="rc-pump">
          <div class="relay-top">
            <div>
              <div class="relay-icon">🚿</div>
              <div class="relay-name">Water Pump</div>
              <div class="relay-pin">GPIO 26 · RELAY_PUMP</div>
            </div>
            <span class="relay-state" id="rs-pump">OFF</span>
          </div>
          <label class="toggle-wrap">
            <div class="toggle">
              <input type="checkbox" id="sw-pump" onchange="setRelay('pump',this.checked)">
              <span class="track"></span>
            </div>
            <span class="toggle-label">Toggle Pump</span>
          </label>
        </div>

      </div>
    </div>

    <!-- ═══ THRESHOLDS PAGE ══════════════════════════════════ -->
    <div class="page" id="pg-thresholds">
      <div class="two-col">
        <div class="thresh-card">
          <div class="section-header">
            <div>
              <div class="section-title">Automation Thresholds</div>
              <div class="section-sub">Thresholds saved to flash (persist across reboots)</div>
            </div>
          </div>

          <div class="thresh-item">
            <div class="thresh-head">
              <span class="thresh-name">🌡 Max Temperature (Fan ON)</span>
              <span class="thresh-val" id="lb-tempMax">--°C</span>
            </div>
            <input type="range" id="sl-tempMax" min="20" max="40" step="0.5"
              oninput="updSlider(this,'lb-tempMax','°C')">
            <div class="thresh-desc">Fan turns ON above this. OFF at (value - 1°C) hysteresis.</div>
          </div>

          <div class="thresh-item">
            <div class="thresh-head">
              <span class="thresh-name">💧 Min Humidity (Mist ON)</span>
              <span class="thresh-val" id="lb-humMin">--%</span>
            </div>
            <input type="range" id="sl-humMin" min="20" max="90" step="1"
              oninput="updSlider(this,'lb-humMin','%')">
            <div class="thresh-desc">Mist ON when humidity drops below this value.</div>
          </div>

          <div class="thresh-item">
            <div class="thresh-head">
              <span class="thresh-name">💧 Max Humidity (Mist OFF)</span>
              <span class="thresh-val" id="lb-humMax">--%</span>
            </div>
            <input type="range" id="sl-humMax" min="20" max="95" step="1"
              oninput="updSlider(this,'lb-humMax','%')">
            <div class="thresh-desc">Mist OFF when humidity exceeds this (+ 5% hysteresis).</div>
          </div>

          <div class="thresh-item">
            <div class="thresh-head">
              <span class="thresh-name">🌱 Min Soil Moisture (Pump ON)</span>
              <span class="thresh-val" id="lb-soilMin">--%</span>
            </div>
            <input type="range" id="sl-soilMin" min="5" max="80" step="1"
              oninput="updSlider(this,'lb-soilMin','%')">
            <div class="thresh-desc">Pump turns ON when soil is drier than this.</div>
          </div>

          <div class="thresh-item">
            <div class="thresh-head">
              <span class="thresh-name">🌱 Max Soil Moisture (Pump OFF)</span>
              <span class="thresh-val" id="lb-soilMax">--%</span>
            </div>
            <input type="range" id="sl-soilMax" min="20" max="100" step="1"
              oninput="updSlider(this,'lb-soilMax','%')">
            <div class="thresh-desc">Pump turns OFF when soil exceeds this (+ 5% hysteresis).</div>
          </div>

          <button class="save-btn" onclick="saveThresholds()">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M19 21H5a2 2 0 01-2-2V5a2 2 0 012-2h11l5 5v11a2 2 0 01-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
            Save to Flash
          </button>
        </div>

        <div class="thresh-card">
          <div class="section-header">
            <div class="section-title">Hysteresis & Anti-flicker</div>
          </div>
          <div style="font-size:.82em;color:var(--muted);line-height:1.7;margin-bottom:16px;">
            The firmware applies fixed hysteresis to prevent relay chattering:
          </div>
          <table style="width:100%;font-size:.8em;border-collapse:collapse;">
            <tr style="border-bottom:1px solid var(--border2)">
              <td style="padding:9px 0;color:var(--muted)">Temperature</td>
              <td style="font-family:var(--mono);color:var(--orange);text-align:right">± 1.0 °C</td>
            </tr>
            <tr style="border-bottom:1px solid var(--border2)">
              <td style="padding:9px 0;color:var(--muted)">Humidity</td>
              <td style="font-family:var(--mono);color:var(--blue);text-align:right">± 5.0 %</td>
            </tr>
            <tr style="border-bottom:1px solid var(--border2)">
              <td style="padding:9px 0;color:var(--muted)">Soil Moisture</td>
              <td style="font-family:var(--mono);color:var(--green);text-align:right">± 5.0 %</td>
            </tr>
            <tr>
              <td style="padding:9px 0;color:var(--muted)">Min relay interval</td>
              <td style="font-family:var(--mono);color:var(--purple);text-align:right">5 000 ms</td>
            </tr>
          </table>
          <div style="margin-top:16px;padding:12px;background:var(--surface2);border-radius:8px;font-size:.75em;color:var(--muted);line-height:1.6;">
            History is sampled every <strong style="color:var(--text)">60 seconds</strong>.<br>
            Live OLED and LED update every <strong style="color:var(--text)">2 seconds</strong>.<br>
            Fail-safe: all relays OFF on sensor error.
          </div>
        </div>
      </div>
    </div>

    <!-- ═══ EVENTS / SCHEDULE PAGE ═══════════════════════════ -->
    <div class="page" id="pg-events">
      <div class="ev-table-wrap">
        <table class="ev-table">
          <thead>
            <tr>
              <th>Type</th><th>Start Time</th><th>Duration</th><th>Next Run</th><th>Enabled</th>
            </tr>
          </thead>
          <tbody id="evTable"></tbody>
        </table>
      </div>

      <div class="timeline-card">
        <div class="section-header">
          <div class="section-title">24-hour Timeline</div>
          <div style="font-size:.72em;color:var(--muted)">Scheduled misting events</div>
        </div>
        <div id="timeline"></div>
        <div class="tl-labels">
          <span>00:00</span><span>06:00</span><span>12:00</span><span>18:00</span><span>24:00</span>
        </div>
      </div>
    </div>

  </div><!-- .content -->
</div><!-- .main -->

<!-- Toast -->
<div class="toast" id="toast"></div>

<script>
// ════════════════════════════════════════════════════════════
//  STATE
// ════════════════════════════════════════════════════════════
const S = {temp:0,hum:0,soil:0,mist:false,fan:false,pump:false,auto:false,connected:false,uptime:0};
const charts = {};
let measTab  = 'temp';
let histData = {temp:[],hum:[],soil:[],epoch:[]};

// Rolling history for home chart
const MAX_LIVE = 20;
const liveQ = {labels:[],temp:[],hum:[],soil:[]};

// ════════════════════════════════════════════════════════════
//  TOAST
// ════════════════════════════════════════════════════════════
function toast(msg, type='ok') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show ' + type;
  setTimeout(() => el.className = 'toast', 2800);
}

// ════════════════════════════════════════════════════════════
//  NAVIGATION
// ════════════════════════════════════════════════════════════
const PAGE_TITLES = {
  home:'Overview', measurements:'Measurements',
  controls:'Controls', thresholds:'Thresholds', events:'Schedule'
};
function showPage(el, id) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
  document.getElementById('pg-' + id).classList.add('active');
  el.classList.add('active');
  document.getElementById('pageTitle').textContent = PAGE_TITLES[id] || id;
  if (id === 'measurements') refreshMeasChart();
  if (id === 'events')       loadEvents();
  if (id === 'thresholds')   loadThresholds();
  if (id === 'controls')     syncRelayToggles();
}

// ════════════════════════════════════════════════════════════
//  API helper
// ════════════════════════════════════════════════════════════
async function api(url, method='GET', body=null) {
  try {
    const opts = {method, headers:{}};
    if (body) { opts.headers['Content-Type']='application/json'; opts.body=JSON.stringify(body); }
    const r = await fetch(url, opts);
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

// ════════════════════════════════════════════════════════════
//  POLLING
// ════════════════════════════════════════════════════════════
async function pollLive() {
  const d = await api('/api/live');
  if (!d) { setConn(false); return; }
  setConn(true);
  Object.assign(S, {
    temp:d.temp||0, hum:d.hum||0, soil:d.soil||0,
    mist:!!d.mist, fan:!!d.fan, pump:!!d.pump,
    auto:!!d.autoMode, uptime:d.uptime||0
  });
  // Update timestamp
  document.getElementById('tsBadge').textContent = d.ts ? d.ts.split('T')[1]||d.ts : '--';
  // Update uptime
  const u = S.uptime;
  const hh=Math.floor(u/3600), mm=Math.floor((u%3600)/60), ss=u%60;
  document.getElementById('uptimeLabel').textContent =
    `${String(hh).padStart(2,'0')}:${String(mm).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;

  renderHome();
  pushLiveQ();
  if (charts.home) { charts.home.data.labels=[...liveQ.labels]; charts.home.update('none'); }
  updateModePill();
  syncRelayToggles();
}

function setConn(v) {
  S.connected = v;
  const dot = document.getElementById('connDot');
  const lbl = document.getElementById('connLabel');
  dot.className = 'dot' + (v ? ' on' : ' err');
  lbl.textContent = v ? 'Connected' : 'Offline';
}

function pushLiveQ() {
  const t = new Date().toLocaleTimeString('en-GB',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
  liveQ.labels.push(t); liveQ.temp.push(S.temp); liveQ.hum.push(S.hum); liveQ.soil.push(S.soil);
  if (liveQ.labels.length > MAX_LIVE) {
    liveQ.labels.shift(); liveQ.temp.shift(); liveQ.hum.shift(); liveQ.soil.shift();
  }
}

// ════════════════════════════════════════════════════════════
//  HOME RENDER
// ════════════════════════════════════════════════════════════
function renderHome() {
  setText('h-temp', S.temp.toFixed(1));
  setText('h-hum',  S.hum.toFixed(0));
  setText('h-soil', S.soil.toFixed(0));
  setText('h-mist', S.mist ? 'ON' : 'OFF');
  setText('h-fan',  S.fan  ? 'ON' : 'OFF');
  setText('h-pump', S.pump ? 'ON' : 'OFF');

  setBadge('h-tempS', S.temp>30?'err':S.temp>27?'warn':'ok',
    S.temp>30?'High':S.temp>27?'Warm':'Normal');
  setBadge('h-humS',  S.hum>80?'warn':S.hum<40?'warn':'ok',
    S.hum>80?'High':S.hum<40?'Low':'Good');
  setBadge('h-soilS', S.soil<30?'warn':S.soil>80?'warn':'ok',
    S.soil<30?'Dry':S.soil>80?'Wet':'Good');
  setBadge('h-mistS', S.mist?'active':'', S.mist?'Running':'Idle');
  setBadge('h-fanS',  S.fan ?'active':'', S.fan ?'Running':'Idle');
  setBadge('h-pumpS', S.pump?'cyan':'',   S.pump?'Running':'Idle');

  // Gauges
  const tempPct = Math.min(100, Math.max(0, (S.temp-15)/30*100));
  const humPct  = S.hum;
  const soilPct = S.soil;
  setText('g-tempV',  S.temp.toFixed(1)+'°C');
  setText('g-humV',   S.hum.toFixed(0)+'%');
  setText('g-soilV',  S.soil+'%');
  setStyle('g-tempF', 'width', tempPct.toFixed(1)+'%');
  setStyle('g-humF',  'width', humPct+'%');
  setStyle('g-soilF', 'width', soilPct+'%');

  // Alerts
  const alerts=[];
  if (!S.connected) alerts.push({type:'err',msg:'ESP32 offline — check WiFi'});
  if (S.temp>30)  alerts.push({type:'err',msg:`Temperature critical: ${S.temp.toFixed(1)}°C`});
  if (S.temp>27)  alerts.push({type:'warn',msg:`Temperature elevated: ${S.temp.toFixed(1)}°C`});
  if (S.hum>80)   alerts.push({type:'warn',msg:`Humidity high: ${S.hum.toFixed(0)}%`});
  if (S.hum<40)   alerts.push({type:'warn',msg:`Humidity low: ${S.hum.toFixed(0)}%`});
  if (S.soil<30)  alerts.push({type:'warn',msg:`Soil dry: ${S.soil}%`});

  const box = document.getElementById('alertsBox');
  box.innerHTML = alerts.map(a=>`
    <div class="alert-strip ${a.type}">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
        <path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/>
        <line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/>
      </svg>${a.msg}</div>`).join('');
}

function setText(id, v) { const e=document.getElementById(id); if(e) e.textContent=v; }
function setStyle(id,p,v){ const e=document.getElementById(id); if(e) e.style[p]=v; }
function setBadge(id, cls, txt) {
  const el = document.getElementById(id);
  if (!el) return;
  el.className = 'kpi-badge' + (cls ? ' '+cls : '');
  el.textContent = txt || '--';
}

// ════════════════════════════════════════════════════════════
//  MODE
// ════════════════════════════════════════════════════════════
function updateModePill() {
  const p = document.getElementById('modePill');
  const t = document.getElementById('modeText');
  p.className = 'mode-pill' + (S.auto ? ' auto' : '');
  t.textContent = S.auto ? 'AUTO' : 'MANUAL';
}
async function toggleMode() {
  await api('/api/mode','POST',{auto:!S.auto});
  S.auto = !S.auto;
  updateModePill();
  toast(S.auto ? '🤖 Auto mode enabled' : '🖐 Manual mode enabled');
}

// ════════════════════════════════════════════════════════════
//  HOME CHART
// ════════════════════════════════════════════════════════════
function initHomeChart() {
  const ctx = document.getElementById('homeChart');
  charts.home = new Chart(ctx, {
    type:'line',
    data:{
      labels: liveQ.labels,
      datasets:[
        { label:'Temp °C', data:liveQ.temp, borderColor:'#ef5350', backgroundColor:'rgba(239,83,80,.08)',
          tension:.4,fill:true,pointRadius:2,borderWidth:1.5,yAxisID:'y' },
        { label:'Humidity %', data:liveQ.hum, borderColor:'#58a6ff', backgroundColor:'rgba(88,166,255,.06)',
          tension:.4,fill:true,pointRadius:2,borderWidth:1.5,yAxisID:'y2' },
        { label:'Soil %', data:liveQ.soil, borderColor:'#3fb950', backgroundColor:'rgba(63,185,80,.06)',
          tension:.4,fill:true,pointRadius:2,borderWidth:1.5,yAxisID:'y2' }
      ]
    },
    options:{
      responsive:true, maintainAspectRatio:false,
      interaction:{mode:'index',intersect:false},
      scales:{
        x:{grid:{color:'rgba(255,255,255,.04)'},ticks:{color:'#7d8590',font:{size:10}}},
        y:{position:'left',min:10,max:45,
          grid:{color:'rgba(255,255,255,.04)'},
          ticks:{color:'#7d8590',font:{size:10}},
          title:{display:true,text:'°C',color:'#7d8590',font:{size:10}}},
        y2:{position:'right',min:0,max:100,
          grid:{drawOnChartArea:false},
          ticks:{color:'#7d8590',font:{size:10}},
          title:{display:true,text:'%',color:'#7d8590',font:{size:10}}}
      },
      plugins:{
        legend:{position:'bottom',labels:{color:'#7d8590',boxWidth:10,font:{size:11},padding:16}},
        tooltip:{backgroundColor:'#1c2128',borderColor:'#30363d',borderWidth:1,
          titleColor:'#e6edf3',bodyColor:'#7d8590',padding:10}
      }
    }
  });
}

// ════════════════════════════════════════════════════════════
//  MEASUREMENTS CHART
// ════════════════════════════════════════════════════════════
const measCfg = {
  temp: {label:'Temperature (°C)', color:'#ef5350', unit:'°C', min:10, max:40, title:'Temperature History'},
  hum:  {label:'Humidity (%)',      color:'#58a6ff', unit:'%',  min:0,  max:100,title:'Humidity History'},
  soil: {label:'Soil Moisture (%)', color:'#3fb950', unit:'%',  min:0,  max:100,title:'Soil Moisture History'}
};

async function refreshMeasChart() {
  const d = await api('/api/history');
  if (!d) return;
  histData = d;
  buildMeasChart();
  computeStats();
}

function switchTab(tab, el) {
  measTab = tab;
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  el.classList.add('active');
  buildMeasChart();
  computeStats();
}

function buildMeasChart() {
  if (charts.meas) charts.meas.destroy();
  const c = measCfg[measTab];
  const labels = (histData.epoch||[]).map(e => {
    const d = new Date(e*1000);
    return d.toLocaleTimeString('en-GB',{hour:'2-digit',minute:'2-digit'});
  });
  const data = histData[measTab] || [];
  setText('measTitle', c.title);

  charts.meas = new Chart(document.getElementById('measChart'), {
    type:'line',
    data:{labels, datasets:[{
      label:c.label, data,
      borderColor:c.color, backgroundColor:c.color+'14',
      tension:.3, fill:true, pointRadius:2, borderWidth:2
    }]},
    options:{
      responsive:true, maintainAspectRatio:false,
      scales:{
        x:{grid:{color:'rgba(255,255,255,.04)'},ticks:{color:'#7d8590',font:{size:10},maxTicksLimit:12}},
        y:{min:c.min,max:c.max,
          grid:{color:'rgba(255,255,255,.04)'},
          ticks:{color:'#7d8590',font:{size:10}}}
      },
      plugins:{
        legend:{display:false},
        tooltip:{backgroundColor:'#1c2128',borderColor:'#30363d',borderWidth:1,
          titleColor:'#e6edf3',bodyColor:'#7d8590',padding:10}
      }
    }
  });
}

function computeStats() {
  const vals = (histData[measTab] || []).map(Number).filter(v => !isNaN(v));
  if (!vals.length) return;
  const min = Math.min(...vals), max = Math.max(...vals);
  const avg = vals.reduce((a,b)=>a+b,0)/vals.length;
  const c = measCfg[measTab];
  setText('st-min', min.toFixed(1)+c.unit);
  setText('st-avg', avg.toFixed(1)+c.unit);
  setText('st-max', max.toFixed(1)+c.unit);
}

// ════════════════════════════════════════════════════════════
//  CONTROLS — RELAY TOGGLES
// ════════════════════════════════════════════════════════════
function syncRelayToggles() {
  const sw = {mist: S.mist, fan: S.fan, pump: S.pump};
  for (const [k,v] of Object.entries(sw)) {
    const el = document.getElementById('sw-'+k);
    if (el) el.checked = v;
    const rs = document.getElementById('rs-'+k);
    if (rs) { rs.className='relay-state'+(v?' on':''); rs.textContent=v?'ON':'OFF'; }
    const rc = document.getElementById('rc-'+k);
    if (rc) rc.className = 'relay-card'+(v?' on':'');
  }
}

async function setRelay(which, on) {
  if (S.auto) {
    toast('Switch to MANUAL mode first', 'err');
    const el = document.getElementById('sw-'+which);
    if (el) el.checked = !on;
    return;
  }
  const r = await api('/api/relay','POST',{relay:which, state:on});
  if (r && r.ok) toast(`${which.toUpperCase()} ${on?'ON':'OFF'}`, on?'ok':'ok');
  else toast('Relay command failed','err');
}

// ════════════════════════════════════════════════════════════
//  THRESHOLDS
// ════════════════════════════════════════════════════════════
async function loadThresholds() {
  const t = await api('/api/thresholds');
  if (!t) return;
  const fields = {
    tempMax:['sl-tempMax','lb-tempMax','°C'],
    humMin: ['sl-humMin', 'lb-humMin','%'],
    humMax: ['sl-humMax', 'lb-humMax','%'],
    soilMin:['sl-soilMin','lb-soilMin','%'],
    soilMax:['sl-soilMax','lb-soilMax','%']
  };
  for (const [k,[sid,lid,unit]] of Object.entries(fields)) {
    const el = document.getElementById(sid);
    if (el) { el.value = t[k]; document.getElementById(lid).textContent = t[k]+unit; }
  }
}

function updSlider(el, lblId, unit) {
  document.getElementById(lblId).textContent = el.value + unit;
}

async function saveThresholds() {
  const g = id => parseFloat(document.getElementById(id).value);
  const r = await api('/api/thresholds','POST',{
    tempMax:g('sl-tempMax'), humMin:g('sl-humMin'), humMax:g('sl-humMax'),
    soilMin:g('sl-soilMin'), soilMax:g('sl-soilMax')
  });
  if (r && r.ok) toast('✅ Thresholds saved to flash','ok');
  else toast('Save failed','err');
}

// ════════════════════════════════════════════════════════════
//  EVENTS
// ════════════════════════════════════════════════════════════
async function loadEvents() {
  const evs = await api('/api/events');
  if (!evs) return;

  const now = new Date();
  const tbody = document.getElementById('evTable');
  tbody.innerHTML = evs.map(e => {
    const hh = String(e.hour).padStart(2,'0');
    const mm = String(e.minute).padStart(2,'0');
    // Compute "next run" label
    const evMin = e.hour*60 + e.minute;
    const nowMin = now.getHours()*60 + now.getMinutes();
    const diffMin = evMin > nowMin ? evMin-nowMin : (1440-nowMin+evMin);
    const dh = Math.floor(diffMin/60), dm = diffMin%60;
    const nextLabel = e.enabled
      ? (diffMin < 60 ? `in ${dm}min` : `in ${dh}h ${dm}min`)
      : '—';
    return `<tr>
      <td><span class="ev-chip">💧 Misting</span></td>
      <td class="time-mono">${hh}:${mm}</td>
      <td>${e.dur} min</td>
      <td style="color:var(--muted)">${nextLabel}</td>
      <td>
        <label class="toggle-wrap">
          <div class="toggle" style="transform:scale(.85)">
            <input type="checkbox" ${e.enabled?'checked':''}
              onchange="toggleEvent(${e.id},this.checked)">
            <span class="track"></span>
          </div>
        </label>
      </td>
    </tr>`;
  }).join('');

  renderTimeline(evs);
}

async function toggleEvent(id, en) {
  const evs = await api('/api/events');
  if (!evs) return;
  const e = evs.find(x => x.id === id);
  if (e) {
    const r = await api('/api/events','POST',{id,hour:e.hour,minute:e.minute,dur:e.dur,enabled:en});
    if (r && r.ok) toast(`Event ${id+1} ${en?'enabled':'disabled'}`,'ok');
  }
}

function renderTimeline(evs) {
  const tl = document.getElementById('timeline');
  const total = 24*60;
  const pct = m => (m/total*100).toFixed(3)+'%';

  tl.innerHTML = `
    <div class="tl-row">
      <span class="tl-label">Mist</span>
      <div class="tl-track">
        ${evs.filter(e=>e.enabled).map(e=>{
          const start = e.hour*60+e.minute;
          const w = Math.max(e.dur/total*100,.5);
          return `<div class="tl-tick" title="${String(e.hour).padStart(2,'0')}:${String(e.minute).padStart(2,'0')} · ${e.dur}min"
            style="left:${pct(start)};width:${w.toFixed(3)}%"></div>`;
        }).join('')}
      </div>
    </div>`;
}

// ════════════════════════════════════════════════════════════
//  INIT
// ════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {
  initHomeChart();
  pollLive();
  setInterval(pollLive, 2000);
});
</script>
</body>
</html>)RAWHTML";

    server.sendHeader("Content-Type", "text/html; charset=utf-8");
    server.send(200, "text/html", html);
}

// ════════════════════════════════════════════════════════════
//  UTILITY
// ════════════════════════════════════════════════════════════
void printBanner() {
    Serial.println("\n============================================");
    Serial.println("  SMART TERRARIUM v4.0 — ESP32");
    Serial.println("  Mist·Fan·Pump · Auto+Manual · NTP · OTA");
    Serial.println("============================================\n");
}

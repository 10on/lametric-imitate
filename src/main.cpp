#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "config.h"

#define MATRIX_PIN       8
#define LIGHT_SENSOR_PIN 2

const char* TOPIC_SUBSCRIBERS  = "lametric/subscribers";
const char* TOPIC_CMD          = "lametric/cmd";
const char* TOPIC_SCROLL       = "lametric/scroll";
const char* TOPIC_BRIGHTNESS   = "lametric/brightness/set";
const char* TOPIC_BRIGHT_STATE = "lametric/brightness/state";
const char* TOPIC_AUTO_SET     = "lametric/brightness_auto/set";
const char* TOPIC_AUTO_STATE   = "lametric/brightness_auto/state";
const char* TOPIC_LIGHT        = "lametric/light";
const char* TOPIC_RESTART      = "lametric/restart";
const char* TOPIC_OTA_SET      = "lametric/ota/set";
const char* TOPIC_OTA_STATE    = "lametric/ota/state";

const unsigned long MQTT_DATA_TIMEOUT      = 5  * 60 * 1000UL;
const unsigned long YT_POLL_INTERVAL       = 60 * 1000UL;
const unsigned long LIGHT_PUBLISH_INTERVAL = 10 * 1000UL;
const unsigned long BRIGHTNESS_INTERVAL    = 500UL;

static bool displayDirty = false;
void markDirty() { displayDirty = true; }
void commitDisplay(Adafruit_NeoMatrix& matrix) {
    if (displayDirty) { matrix.show(); displayDirty = false; }
}

String subscriberCount   = "";
bool   brightnessAuto    = true;
bool   otaEnabled        = false;
int    manualBrightness  = 40;
int    currentBrightness = 40;

unsigned long lastMqttDataTime     = 0;
unsigned long lastYtPollTime       = 0;
unsigned long lastLightPublishTime = 0;
unsigned long lastBrightnessUpdate = 0;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
    32, 8, MATRIX_PIN,
    NEO_MATRIX_BOTTOM + NEO_MATRIX_RIGHT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
    NEO_GRB + NEO_KHZ800
);

// --- Scroll ---

struct {
    String        text;
    uint16_t      colors[2];
    int           colorCount  = 1;
    int           colorPhase  = 0;
    int           x           = 0;
    bool          active      = false;
    bool          loop        = false;
    unsigned long stepMs      = 40;
    unsigned long lastStep    = 0;
    int           textWidth   = 0;
} scroll;

// --- Digit animation ---

enum DigitAnimType { ANIM_NONE, ANIM_VERTICAL, ANIM_HORIZONTAL };

struct {
    DigitAnimType type       = ANIM_NONE;
    String        oldText;
    String        newText;
    int           prefixLen  = 0;
    int           offset     = 0;
    int           maxOffset  = 0;
    bool          active     = false;
    unsigned long lastStep   = 0;
    unsigned long stepMs     = 30;
} digitAnim;

// x-pixel position of character at index i (layout: 0→9, 1→14, 2→20, 3→26)
int digitCharX(int i) {
    return (i == 0) ? 9 : (8 + i * 6);
}

void cmdLogo();  // forward declaration for milestone

// --- Milestone dot tracer ---

// Adafruit GFX default 5x7 font for digits: 5 bytes per digit,
// each byte is one column, bit0=top row, bit6=bottom row.
static const uint8_t DIGIT_FONT[10][5] PROGMEM = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
};

#define MILESTONE_PATH_MAX 150
#define MILESTONE_TRAIL    3   // positions behind head that show colored tail

struct {
    bool   active;
    int    pathIdx;
    int    dir;          // +1 forward, -1 backward
    int    pathLen;
    int8_t pathX[MILESTONE_PATH_MAX];
    int8_t pathY[MILESTONE_PATH_MAX];
    unsigned long lastStep;
    unsigned long passStartTime;
    int    pass;         // 0=purple/pink, 1=cyan/lime
} milestone = {false};

uint16_t milestoneTrailColor(int t, int pass) {
    if (pass == 0) {
        if (t == 0) return matrix.Color(160,   0, 255);  // purple head
        if (t == 1) return matrix.Color(220,   0, 180);  // purple-pink
        if (t == 2) return matrix.Color(255,  80, 140);  // pink
                    return matrix.Color(255, 180, 220);  // light pink → merges into white
    } else {
        if (t == 0) return matrix.Color(  0, 220, 255);  // cyan head
        if (t == 1) return matrix.Color(  0, 255, 140);  // teal-green
        if (t == 2) return matrix.Color( 80, 255,  60);  // lime
                    return matrix.Color(180, 255, 180);  // light lime → merges into white
    }
}

// Enumerate all lit pixels of the current subscriber count into milestone.path*
void buildMilestonePath() {
    milestone.pathLen = 0;
    const char* s = subscriberCount.c_str();
    int len = (int)strlen(s);
    for (int ci = 0; ci < len && ci < 4; ci++) {
        if (!isdigit((unsigned char)s[ci])) continue;
        int d  = s[ci] - '0';
        int cx = digitCharX(ci);
        for (int col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&DIGIT_FONT[d][col]);
            for (int row = 0; row < 7; row++) {
                if ((bits >> row) & 1) {
                    int px = cx + col;
                    if (px >= 9 && px <= 31 && milestone.pathLen < MILESTONE_PATH_MAX) {
                        milestone.pathX[milestone.pathLen] = (int8_t)px;
                        milestone.pathY[milestone.pathLen] = (int8_t)row;
                        milestone.pathLen++;
                    }
                }
            }
        }
    }
}

void startMilestone() {
    buildMilestonePath();
    if (milestone.pathLen == 0) return;
    milestone.pathIdx       = 0;
    milestone.dir           = 1;
    milestone.pass          = 0;
    milestone.active        = true;
    milestone.lastStep      = 0;
    milestone.passStartTime = millis();
}

void drawDigitsDirect() {
    if (subscriberCount.length() == 0) return;
    const char* count = subscriberCount.c_str();
    char rest[10];
    strncpy(rest, count + 1, sizeof(rest) - 1);
    rest[sizeof(rest) - 1] = '\0';
    matrix.fillRect(9, 0, 23, 8, matrix.Color(0, 0, 0));
    matrix.setTextColor(matrix.Color(255, 255, 255));
    matrix.setCursor(9, 0);  matrix.print(count[0]);
    matrix.setCursor(14, 0); matrix.print(rest);
}

void tickMilestone() {
    if (!milestone.active || scroll.active || digitAnim.active) return;

    unsigned long now = millis();
    if (now - milestone.lastStep < 40UL) return;
    milestone.lastStep = now;

    // Each pass runs 15s; two passes = 30s total
    if (now - milestone.passStartTime >= 15000UL) {
        if (milestone.pass == 0) {
            milestone.pass          = 1;
            milestone.passStartTime = now;
        } else {
            milestone.active = false;
            return;
        }
    }

    drawDigitsDirect();

    // Draw trail oldest-first so head (t=0) is painted last and lands on top
    for (int t = MILESTONE_TRAIL; t >= 0; t--) {
        int idx = milestone.pathIdx - t * milestone.dir;
        if (idx < 0 || idx >= milestone.pathLen) continue;
        matrix.drawPixel(milestone.pathX[idx], milestone.pathY[idx],
                         milestoneTrailColor(t, milestone.pass));
    }

    cmdLogo();
    markDirty();

    // Advance head; bounce at both ends
    milestone.pathIdx += milestone.dir;
    if (milestone.pathIdx >= milestone.pathLen) {
        milestone.pathIdx = milestone.pathLen - 2;
        milestone.dir     = -1;
    } else if (milestone.pathIdx < 0) {
        milestone.pathIdx = 1;
        milestone.dir     = 1;
    }
}

// Forward declarations
void restoreDisplay();
void displayCount(const char* count);
void cmdLogo();

void startDigitAnim(const char* oldStr, const char* newStr) {
    if (!oldStr || strlen(oldStr) == 0 || !newStr || strlen(newStr) == 0) {
        displayCount(newStr);
        return;
    }

    int oldLen = strlen(oldStr);
    int newLen = strlen(newStr);

    // Find common prefix from left
    int prefixLen = 0;
    int minLen = min(oldLen, newLen);
    while (prefixLen < minLen && oldStr[prefixLen] == newStr[prefixLen])
        prefixLen++;

    int newSuffixLen = newLen - prefixLen;
    if (newSuffixLen == 0) { displayCount(newStr); return; }

    digitAnim.oldText   = oldStr;
    digitAnim.newText   = newStr;
    digitAnim.prefixLen = prefixLen;
    digitAnim.offset    = 0;
    digitAnim.lastStep  = 0;
    digitAnim.active    = true;
    digitAnim.stepMs    = 40;

    if (newSuffixLen == 1) {
        digitAnim.type      = ANIM_VERTICAL;
        digitAnim.maxOffset = 9;
    } else if (newSuffixLen == 2) {
        digitAnim.type      = ANIM_HORIZONTAL;
        digitAnim.maxOffset = 12;  // 2 chars × 6px
    } else if (newSuffixLen == 3) {
        digitAnim.type      = ANIM_VERTICAL;  // all 3 digits slide up together
        digitAnim.maxOffset = 9;
    } else {
        // thousands — no animation yet
        digitAnim.active = false;
        displayCount(newStr);
        return;
    }
}

void tickDigitAnim() {
    if (!digitAnim.active || scroll.active) return;

    unsigned long now = millis();
    if (now - digitAnim.lastStep < digitAnim.stepMs) return;
    digitAnim.lastStep = now;

    uint16_t white = matrix.Color(255, 255, 255);
    uint16_t black = matrix.Color(0, 0, 0);
    int off        = digitAnim.offset;
    int prefixLen  = digitAnim.prefixLen;
    int oldLen     = digitAnim.oldText.length();
    int newLen     = digitAnim.newText.length();
    int oldSuffLen = oldLen - prefixLen;
    int newSuffLen = newLen - prefixLen;

    // Clear number area
    matrix.fillRect(9, 0, 23, 8, black);

    matrix.setTextColor(white);

    if (digitAnim.type == ANIM_VERTICAL) {
        int maxLen = max(oldLen, newLen);
        // All suffix digits slide up simultaneously
        for (int i = prefixLen; i < maxLen; i++) {
            int x = digitCharX(i);
            if (i < oldLen) {
                matrix.setCursor(x, 0 - off);
                matrix.print(digitAnim.oldText[i]);
            }
            if (i < newLen) {
                matrix.setCursor(x, 8 - off);
                matrix.print(digitAnim.newText[i]);
            }
        }
        // Prefix on top
        for (int i = 0; i < prefixLen; i++) {
            matrix.setCursor(digitCharX(i), 0);
            matrix.print(digitAnim.newText[i]);
        }
    } else {
        int totalSuffWidth = newSuffLen * 6;
        // Old suffix slides left — drawn first so prefix covers it as it exits
        for (int i = 0; i < oldSuffLen; i++) {
            int x = digitCharX(prefixLen + i) - off;
            matrix.setCursor(x, 0);
            matrix.print(digitAnim.oldText[prefixLen + i]);
        }
        // New suffix enters from the right
        for (int i = 0; i < newSuffLen; i++) {
            int x = digitCharX(prefixLen + i) + totalSuffWidth - off;
            matrix.setCursor(x, 0);
            matrix.print(digitAnim.newText[prefixLen + i]);
        }
        // Blank the prefix zone then redraw prefix — cleanly clips exiting digits
        if (prefixLen > 0) {
            int prefixEndX = digitCharX(prefixLen - 1) + 6;
            matrix.fillRect(9, 0, prefixEndX - 9, 8, matrix.Color(0, 0, 0));
            for (int i = 0; i < prefixLen; i++) {
                matrix.setCursor(digitCharX(i), 0);
                matrix.print(digitAnim.newText[i]);
            }
        }
    }

    // Redraw logo last — masks any overflow into logo area (x=0..8)
    cmdLogo();
    markDirty();

    digitAnim.offset++;
    if (digitAnim.offset >= digitAnim.maxOffset) {
        digitAnim.active = false;
        displayCount(digitAnim.newText.c_str());
    }
}

// --- Display helpers ---

uint16_t colorFromJson(JsonArrayConst arr, uint16_t fallback) {
    if (arr.size() < 3) return fallback;
    return matrix.Color(arr[0].as<uint8_t>(), arr[1].as<uint8_t>(), arr[2].as<uint8_t>());
}

void cmdLogo() {
    uint16_t red   = matrix.Color(255, 0, 0);
    uint16_t white = matrix.Color(255, 255, 255);
    uint16_t black = matrix.Color(0, 0, 0);
    uint16_t logo[8][9] = {
        {black, red,   red,   red,   red,   red,   red,   red,   black},
        {red,   red,   red,   white, red,   red,   red,   red,   red  },
        {red,   red,   red,   white, white, red,   red,   red,   red  },
        {red,   red,   red,   white, white, white, red,   red,   red  },
        {red,   red,   red,   white, white, red,   red,   red,   red  },
        {red,   red,   red,   white, red,   red,   red,   red,   red  },
        {black, red,   red,   red,   red,   red,   red,   red,   black},
        {black, black, black, black, black, black, black, black, black}
    };
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 9; x++)
            matrix.drawPixel(x, y, logo[y][x]);
    markDirty();
}

void cmdText(JsonObjectConst obj) {
    int x = obj["x"] | 9;
    int y = obj["y"] | 0;
    uint16_t color = colorFromJson(obj["color"].as<JsonArrayConst>(), matrix.Color(255, 255, 255));
    matrix.setTextColor(color);
    matrix.setCursor(x, y);
    matrix.print(obj["text"] | "");
    markDirty();
}

void cmdRect(JsonObjectConst obj) {
    uint16_t color = colorFromJson(obj["color"].as<JsonArrayConst>(), matrix.Color(0, 0, 0));
    matrix.fillRect(obj["x"] | 0, obj["y"] | 0, obj["w"] | 1, obj["h"] | 1, color);
    markDirty();
}

void cmdPixel(JsonObjectConst obj) {
    uint16_t color = colorFromJson(obj["color"].as<JsonArrayConst>(), matrix.Color(0, 0, 0));
    matrix.drawPixel(obj["x"] | 0, obj["y"] | 0, color);
    markDirty();
}

void cmdClear(JsonObjectConst obj) {
    const char* region = obj["region"] | "all";
    if (strcmp(region, "right") == 0)
        matrix.fillRect(9, 0, 23, 8, matrix.Color(0, 0, 0));
    else
        matrix.fillScreen(matrix.Color(0, 0, 0));
    markDirty();
}

void displayCount(const char* count) {
    if (!count || strlen(count) == 0) return;
    if (scroll.active) return;
    char rest[10];
    strncpy(rest, count + 1, sizeof(rest) - 1);
    rest[sizeof(rest) - 1] = '\0';
    matrix.setTextColor(matrix.Color(255, 255, 255));
    matrix.fillRect(9, 0, 23, 8, matrix.Color(0, 0, 0));
    matrix.setCursor(9, 0);
    matrix.print(count[0]);
    matrix.setCursor(14, 0);
    matrix.print(rest);
    markDirty();
}

void restoreDisplay() {
    if (subscriberCount.length() > 0)
        displayCount(subscriberCount.c_str());
    else
        cmdLogo();
}

// --- Scroll ---

void startScroll(const String& text, uint16_t color, bool loop = false, unsigned long stepMs = 40) {
    scroll.text       = text;
    scroll.colors[0]  = color;
    scroll.colorCount = 1;
    scroll.colorPhase = 0;
    scroll.x          = 32;
    scroll.active     = true;
    scroll.loop       = loop;
    scroll.stepMs     = stepMs;
    scroll.lastStep   = 0;
    scroll.textWidth  = text.length() * 6;
}

void startScrollDual(const String& text, uint16_t c1, uint16_t c2, unsigned long stepMs = 50) {
    startScroll(text, c1, true, stepMs);
    scroll.colors[1]  = c2;
    scroll.colorCount = 2;
}

void stopScroll() {
    scroll.active = false;
    restoreDisplay();
}

void tickScroll() {
    if (!scroll.active) return;
    unsigned long now = millis();
    if (now - scroll.lastStep < scroll.stepMs) return;
    scroll.lastStep = now;

    matrix.fillScreen(matrix.Color(0, 0, 0));
    matrix.setTextColor(scroll.colors[scroll.colorPhase]);
    matrix.setCursor(scroll.x, 0);
    matrix.print(scroll.text);
    markDirty();

    scroll.x--;
    if (scroll.x < -scroll.textWidth) {
        if (scroll.loop) {
            scroll.x         = 32;
            scroll.colorPhase = (scroll.colorPhase + 1) % scroll.colorCount;
        } else {
            scroll.active = false;
            restoreDisplay();
        }
    }
}

// --- Brightness ---

void updateBrightness() {
    if (!brightnessAuto) return;
    static int lastBrightness = -1;
    int raw = analogRead(LIGHT_SENSOR_PIN);
    // max 98 (~30% less than 140) — LDR is binary in current conditions, SFH309FA pending
    int brightness = map(raw, 0, 4095, 98, 5);
    if (abs(brightness - lastBrightness) > 2) {
        lastBrightness    = brightness;
        currentBrightness = brightness;
        matrix.setBrightness(brightness);
        markDirty();
    }
}

int readLightPercent() {
    return map(analogRead(LIGHT_SENSOR_PIN), 0, 4095, 100, 0);
}

void publishBrightnessState();

// --- Commands ---

void handleCmd(const char* payload, unsigned int length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;
    const char* cmd = doc["cmd"] | "";
    if      (strcmp(cmd, "text")            == 0) cmdText(doc.as<JsonObjectConst>());
    else if (strcmp(cmd, "rect")            == 0) cmdRect(doc.as<JsonObjectConst>());
    else if (strcmp(cmd, "pixel")           == 0) cmdPixel(doc.as<JsonObjectConst>());
    else if (strcmp(cmd, "clear")           == 0) cmdClear(doc.as<JsonObjectConst>());
    else if (strcmp(cmd, "logo")            == 0) cmdLogo();
    else if (strcmp(cmd, "scroll")          == 0) {
        String text = doc["text"] | "";
        if (text.length() > 0) {
            uint16_t color = colorFromJson(doc["color"].as<JsonArrayConst>(), matrix.Color(255, 255, 255));
            bool loop      = doc["loop"] | false;
            int  speed     = doc["speed"] | 40;
            startScroll(text, color, loop, speed);
        }
    }
    else if (strcmp(cmd, "scroll_stop")     == 0) stopScroll();
    else if (strcmp(cmd, "brightness_auto") == 0) { brightnessAuto = true; publishBrightnessState(); }
    else if (strcmp(cmd, "brightness")      == 0) {
        brightnessAuto    = false;
        manualBrightness  = doc["value"] | 40;
        currentBrightness = manualBrightness;
        matrix.setBrightness(manualBrightness);
        markDirty();
        publishBrightnessState();
    }
    else if (strcmp(cmd, "restart") == 0) { ESP.restart(); }
}

// --- MQTT ---

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    char val[64] = {};
    if (length < sizeof(val)) memcpy(val, payload, length);

    if (strcmp(topic, TOPIC_SUBSCRIBERS) == 0) {
        String oldCount  = subscriberCount;
        subscriberCount  = val;
        lastMqttDataTime = millis();
        int newNum = atoi(subscriberCount.c_str());
        if (newNum > 0 && newNum % 100 == 0) startMilestone();
        if (!scroll.active) {
            if (oldCount.length() > 0 && oldCount != subscriberCount)
                startDigitAnim(oldCount.c_str(), subscriberCount.c_str());
            else
                displayCount(subscriberCount.c_str());
        }

    } else if (strcmp(topic, TOPIC_CMD) == 0) {
        handleCmd((const char*)payload, length);

    } else if (strcmp(topic, TOPIC_SCROLL) == 0) {
        if (length > 0) {
            String text = String(val);
            startScroll(text, matrix.Color(255, 255, 255), false, 40);
        }

    } else if (strcmp(topic, TOPIC_BRIGHTNESS) == 0) {
        manualBrightness  = atoi(val);
        brightnessAuto    = false;
        currentBrightness = manualBrightness;
        matrix.setBrightness(manualBrightness);
        markDirty();
        publishBrightnessState();

    } else if (strcmp(topic, TOPIC_AUTO_SET) == 0) {
        brightnessAuto = (strcmp(val, "ON") == 0);
        if (!brightnessAuto) {
            currentBrightness = manualBrightness;
            matrix.setBrightness(manualBrightness);
            markDirty();
        }
        publishBrightnessState();

    } else if (strcmp(topic, TOPIC_RESTART) == 0) {
        ESP.restart();

    } else if (strcmp(topic, TOPIC_OTA_SET) == 0) {
        otaEnabled = (strcmp(val, "ON") == 0);
        mqtt.publish(TOPIC_OTA_STATE, otaEnabled ? "ON" : "OFF", true);
        Serial.printf("OTA %s\n", otaEnabled ? "enabled" : "disabled");
        if (otaEnabled) {
            String msg = String("OTA ") + WiFi.localIP().toString();
            startScrollDual(msg, matrix.Color(0, 255, 120), matrix.Color(180, 0, 255));
        } else {
            stopScroll();
        }
    }
}

void publishHaDiscovery() {
    JsonDocument doc;
    char buf[512];

    auto setDevice = [&]() {
        doc["device"]["ids"]  = "lametric_counter";
        doc["device"]["name"] = "LaMetric Counter";
    };

    doc["name"]      = "Brightness";
    doc["unique_id"] = "lametric_brightness";
    doc["cmd_t"]     = TOPIC_BRIGHTNESS;
    doc["stat_t"]    = TOPIC_BRIGHT_STATE;
    doc["min"]       = 5;
    doc["max"]       = 140;
    doc["step"]      = 1;
    setDevice();
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("homeassistant/number/lametric/brightness/config", buf, true);
    doc.clear();

    doc["name"]      = "Auto Brightness";
    doc["unique_id"] = "lametric_brightness_auto";
    doc["cmd_t"]     = TOPIC_AUTO_SET;
    doc["stat_t"]    = TOPIC_AUTO_STATE;
    setDevice();
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("homeassistant/switch/lametric/brightness_auto/config", buf, true);
    doc.clear();

    doc["name"]      = "Light Level";
    doc["unique_id"] = "lametric_light";
    doc["stat_t"]    = TOPIC_LIGHT;
    doc["unit_of_measurement"] = "%";
    setDevice();
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("homeassistant/sensor/lametric/light/config", buf, true);
    doc.clear();

    doc["name"]          = "Restart";
    doc["unique_id"]     = "lametric_restart";
    doc["cmd_t"]         = TOPIC_RESTART;
    doc["payload_press"] = "1";
    setDevice();
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("homeassistant/button/lametric/restart/config", buf, true);
    doc.clear();

    doc["name"]      = "OTA Update";
    doc["unique_id"] = "lametric_ota";
    doc["cmd_t"]     = TOPIC_OTA_SET;
    doc["stat_t"]    = TOPIC_OTA_STATE;
    setDevice();
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("homeassistant/switch/lametric/ota/config", buf, true);
}

void publishBrightnessState() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", manualBrightness);
    mqtt.publish(TOPIC_BRIGHT_STATE, buf, true);
    mqtt.publish(TOPIC_AUTO_STATE, brightnessAuto ? "ON" : "OFF", true);
}

void mqttReconnect() {
    if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt < 10000UL) return;
    lastAttempt = millis();

    Serial.print("MQTT connecting...");
    if (mqtt.connect("lametric-counter", MQTT_USER, MQTT_PASS,
                     "lametric/availability", 1, true, "offline")) {
        mqtt.publish("lametric/availability", "online", true);
        Serial.println(" OK");
        mqtt.subscribe(TOPIC_SUBSCRIBERS);
        mqtt.subscribe(TOPIC_CMD);
        mqtt.subscribe(TOPIC_SCROLL);
        mqtt.subscribe(TOPIC_BRIGHTNESS);
        mqtt.subscribe(TOPIC_AUTO_SET);
        mqtt.subscribe(TOPIC_RESTART);
        mqtt.subscribe(TOPIC_OTA_SET);
        publishHaDiscovery();
        publishBrightnessState();
        mqtt.publish(TOPIC_OTA_STATE, "OFF", true);
    } else {
        Serial.printf(" fail rc=%d\n", mqtt.state());
    }
}

void publishLight() {
    static int lastLight = -1;
    int raw   = analogRead(LIGHT_SENSOR_PIN);
    int light = map(raw, 0, 4095, 100, 0);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", raw);
    mqtt.publish("lametric/light/raw", buf, true);
    if (abs(light - lastLight) < 3) return;
    lastLight = light;
    snprintf(buf, sizeof(buf), "%d", light);
    mqtt.publish(TOPIC_LIGHT, buf, true);
}

static volatile bool ytFetchPending = false;
static String        ytFetchResult  = "";
static SemaphoreHandle_t ytMutex    = nullptr;

void ytFetchTask(void*) {
    HTTPClient http;
    String url = String("https://www.googleapis.com/youtube/v3/channels?part=statistics&id=")
                 + YT_CHANNEL_ID + "&key=" + YT_API_KEY;
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();
    if (code > 0) {
        JsonDocument doc;
        deserializeJson(doc, http.getStream());
        const char* count = doc["items"][0]["statistics"]["subscriberCount"];
        if (count) {
            xSemaphoreTake(ytMutex, portMAX_DELAY);
            ytFetchResult = count;
            xSemaphoreGive(ytMutex);
        }
    } else {
        Serial.printf("HTTP error: %d\n", code);
    }
    http.end();
    ytFetchPending = false;
    vTaskDelete(nullptr);
}

void fetchFromYouTube() {
    if (ytFetchPending) return;
    ytFetchPending = true;
    xTaskCreate(ytFetchTask, "ytfetch", 8192, nullptr, 1, nullptr);
}

void applyYtResult() {
    if (ytFetchResult.isEmpty()) return;
    xSemaphoreTake(ytMutex, portMAX_DELAY);
    String result = ytFetchResult;
    ytFetchResult = "";
    xSemaphoreGive(ytMutex);

    String oldCount = subscriberCount;
    subscriberCount = result;
    Serial.printf("YT subscribers: %s\n", subscriberCount.c_str());
    int newNum = atoi(subscriberCount.c_str());
    if (newNum > 0 && newNum % 100 == 0) startMilestone();
    if (!scroll.active) {
        if (oldCount.length() > 0 && oldCount != subscriberCount)
            startDigitAnim(oldCount.c_str(), subscriberCount.c_str());
        else
            displayCount(subscriberCount.c_str());
    }
}

void setup() {
    Serial.begin(115200);
    matrix.begin();
    matrix.setTextWrap(false);
    matrix.setTextColor(matrix.Color(255, 255, 255));
    matrix.setBrightness(40);
    cmdLogo();
    commitDisplay(matrix);

    ytMutex = xSemaphoreCreateMutex();

    Serial.print("WiFi connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    WiFi.setSleep(false);  // disable modem sleep — prevents interrupt bursts that glitch NeoPixel
    Serial.println(" OK");

    ArduinoOTA.setHostname("lametric-counter");
    ArduinoOTA.begin();

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);
    mqtt.setKeepAlive(90);  // survive 30s milestone animation without MQTT disconnect
    mqttReconnect();

    fetchFromYouTube();
    lastYtPollTime = millis();
}

void loop() {
    bool animating = digitAnim.active || milestone.active;

    if (!animating) {
        if (otaEnabled) ArduinoOTA.handle();
        mqtt.loop();
        mqttReconnect();

        unsigned long now = millis();

        if (now - lastBrightnessUpdate >= BRIGHTNESS_INTERVAL) {
            lastBrightnessUpdate = now;
            updateBrightness();
        }
        if (now - lastLightPublishTime >= LIGHT_PUBLISH_INTERVAL) {
            lastLightPublishTime = now;
            if (mqtt.connected()) publishLight();
        }
        bool mqttFresh = lastMqttDataTime > 0 && (now - lastMqttDataTime < MQTT_DATA_TIMEOUT);
        if (!mqttFresh && (now - lastYtPollTime >= YT_POLL_INTERVAL)) {
            lastYtPollTime = now;
            fetchFromYouTube();
        }
        applyYtResult();
    }

    tickScroll();
    tickDigitAnim();
    tickMilestone();
    commitDisplay(matrix);
}

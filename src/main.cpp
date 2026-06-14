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

void restoreDisplay();

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

// --- Brightness ---

void updateBrightness() {
    if (!brightnessAuto) return;
    static int lastBrightness = -1;
    int raw = analogRead(LIGHT_SENSOR_PIN);
    // max 98 (~30% less than 140) — LDR is binary in current conditions, SFH309FA pending
    int brightness = map(raw, 0, 4095, 98, 5);
    if (abs(brightness - lastBrightness) > 2) {
        lastBrightness   = brightness;
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
        subscriberCount  = val;
        lastMqttDataTime = millis();
        displayCount(subscriberCount.c_str());

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

void fetchFromYouTube() {
    Serial.println("Fetching YouTube API...");
    HTTPClient http;
    String url = String("https://www.googleapis.com/youtube/v3/channels?part=statistics&id=")
                 + YT_CHANNEL_ID + "&key=" + YT_API_KEY;
    http.begin(url);
    int code = http.GET();
    if (code > 0) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        const char* count = doc["items"][0]["statistics"]["subscriberCount"];
        if (count) {
            subscriberCount = count;
            Serial.printf("YT subscribers: %s\n", subscriberCount.c_str());
            displayCount(count);
        }
    } else {
        Serial.printf("HTTP error: %d\n", code);
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    matrix.begin();
    matrix.setTextWrap(false);
    matrix.setTextColor(matrix.Color(255, 255, 255));
    matrix.setBrightness(40);
    cmdLogo();
    commitDisplay(matrix);

    Serial.print("WiFi connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(" OK");

    ArduinoOTA.setHostname("lametric-counter");
    ArduinoOTA.begin();

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);
    mqttReconnect();

    fetchFromYouTube();
    lastYtPollTime = millis();
}

void loop() {
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

    tickScroll();
    commitDisplay(matrix);
}

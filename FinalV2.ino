/*
 * ============================================================
 * Crowd Counter — Dual-Core Real IR Mode + MQ-2 + KY-038
 * ============================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
// WIFI CONNECTION
// ============================================================
const char* WIFI_SSID      = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";
const char* FIREBASE_URL    = "https://crowd-counter-uu-default-rtdb.europe-west1.firebasedatabase.app";

// ============================================================
// Pin Configuration
// ============================================================
const int LEFT_SENSOR_PIN   = 1;    // Left IR sensor (GPIO 5)
const int RIGHT_SENSOR_PIN  = 2;    // Right IR sensor (GPIO 6)
const int MQ2_AO_PIN        = 4;    // Analog output of MQ-2
const int SOUND_PIN         = 3;    // Analog output of KY-038
const int LEFT_LED_PIN      = 13;
const int RIGHT_LED_PIN     = 12;
const int SDA_SCREEN_PIN    = 10;
const int SCL_SCREEN_PIN    = 11;


// ============================================================
// OLED + LED Configuration
// ============================================================

bool leftMode = true;
int lastDisplayedCount = -1;  // Force first update

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);


void setupO_Leds() {
    pinMode(LEFT_LED_PIN, OUTPUT);
    pinMode(RIGHT_LED_PIN, OUTPUT);
    digitalWrite(LEFT_LED_PIN, HIGH);
    digitalWrite(RIGHT_LED_PIN, HIGH);
    Serial.print("  [LED] LEFT_LED_PIN = GPIO ");
    Serial.println(LEFT_LED_PIN);
    Serial.print("  [LED] RIGHT_LED_PIN = GPIO ");
    Serial.println(RIGHT_LED_PIN);

    Wire.begin(SDA_SCREEN_PIN, SCL_SCREEN_PIN);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED not found");
        while (true);
    }

    display.clearDisplay();
    display.display();
}

// ============================================================
// Noise Sensor Configuration
// ============================================================
const int sampleWindow      = 50;   // Sample window width in mS
float smoothedDb            = 40.0;

// ============================================================
// MQ-2 Gas Sensor Configuration
// ============================================================

// ── Voltage divider ─────────────────────────────────────────
const float R_TOP          = 10000.0;  // R1: AOUT → ADC pin (Ω)
const float R_BOT          = 20000.0;  // R2: ADC pin → GND  (Ω)
const float DIVIDER_RATIO  = (R_TOP + R_BOT) / R_BOT;  // ≈ 1.545

// ── Voltages ─────────────────────────────────────────────────
const float SENSOR_VCC     = 5.0;    // MQ-2 supply voltage (V)
const float ADC_VREF       = 3.3;    // ESP32 ADC reference (V)
const float ADC_RESOLUTION = 4095.0; // 12-bit ADC

// ── Load resistor ────────────────────────────────────────────
const float RL_VALUE       = 1.0;    // kΩ

// ── Calibration ──────────────────────────────────────────────
const float RO_CLEAN_AIR   = 9.83;

// ── Smoothing & oversampling ─────────────────────────────────
const float EMA_ALPHA      = 0.1;
const int   OVERSAMPLE     = 64;

// ── Gas curves (log-log, from MQ-2 datasheet) ────────────────
const float LPGCurve[3]    = { 2.3, 0.21, -0.47 };
const float COCurve[3]     = { 2.3, 0.72, -0.34 };
const float SmokeCurve[3]  = { 2.3, 0.53, -0.44 };

float Ro           = RO_CLEAN_AIR;
float lpgSmooth    = 0;
float coSmooth     = 0;
float smokeSmooth  = 0;
float eco2Smooth   = 400.0;
bool  firstReading = true;

// ============================================================
// State Machine (IR Beams)
// ============================================================
enum CrossingState { IDLE, LEFT_FIRST, RIGHT_FIRST };
CrossingState state = IDLE;
unsigned long stateEnteredAt = 0;

const unsigned long CROSSING_TIMEOUT = 3000;
const unsigned long DEBOUNCE_MS      = 30;
const unsigned long COOLDOWN_MS      = 400;

int leftReading      = 1;
int rightReading     = 1;
int lastRawLeft      = 1;
int lastRawRight     = 1;
unsigned long lastLeftChange  = 0;
unsigned long lastRightChange = 0;

volatile int currentCount = 0;
volatile int totalIn      = 0;
volatile int totalOut     = 0;

int uploadedIn  = 0;
int uploadedOut = 0;

unsigned long lastDetectionTime = 0;

TaskHandle_t SensorTask;

void sensorTaskcode(void * parameter);

// ============================================================
// MQ-2 Helper Functions
// ============================================================

// Reads Rs (sensor resistance in kΩ), corrected for the voltage divider
// and the actual 5V sensor supply.
float readRsOversampled(int samples = OVERSAMPLE) {
    long sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += analogRead(MQ2_AO_PIN);
        delayMicroseconds(500);
    }
    float avgRaw = sum / (float)samples;

    // Voltage at the ESP32 ADC pin (after divider)
    float v_adc = (avgRaw / ADC_RESOLUTION) * ADC_VREF;

    // Reconstruct the actual AOUT voltage the MQ-2 is producing
    float v_aout = v_adc * DIVIDER_RATIO;

    // Guard against 0V (open circuit) or rail (sensor saturated)
    if (v_aout <= 0.001f || v_aout >= SENSOR_VCC) return 0.0f;

    // Rs formula: Rs = (VCC − Vout) / Vout × RL
    // VCC here is SENSOR_VCC (5V), NOT the ADC reference
    return ((SENSOR_VCC - v_aout) / v_aout) * RL_VALUE;
}

float getPPM(float rsRo, const float *curve) {
    if (rsRo <= 0) return 0;
    return pow(10, (((log10(rsRo) - curve[1]) / curve[2]) + curve[0]));
}

float emaFilter(float newVal, float prevVal, float alpha = EMA_ALPHA) {
    if (firstReading) return newVal;
    return (alpha * newVal) + ((1.0 - alpha) * prevVal);
}

void readAllGases() {
    float rs   = readRsOversampled();
    float rsRo = (Ro > 0) ? rs / Ro : 0;

    lpgSmooth   = emaFilter(getPPM(rsRo, LPGCurve),   lpgSmooth);
    coSmooth    = emaFilter(getPPM(rsRo, COCurve),    coSmooth);
    smokeSmooth = emaFilter(getPPM(rsRo, SmokeCurve), smokeSmooth);

    // Estimate eCO2 from CO fluctuations (rough proxy)
    
    float targetEco2 = 400.0f + ((coSmooth - 1.0f) * 100.0f);
    if (targetEco2 < 400.0f) targetEco2 = 400.0f;
    if (targetEco2 > 5000.0f) targetEco2 = 5000.0f;  // Cap at 5000 PPM
    eco2Smooth = emaFilter(targetEco2, eco2Smooth, 0.2f);

    firstReading = false;

    Serial.println("─────────────────────────────────");
    Serial.print("  [MQ-2] Rs:     "); Serial.print(rs, 4);          Serial.println(" kΩ");
    Serial.print("  [MQ-2] Rs/Ro:  "); Serial.println(rsRo, 4);
    Serial.print("  [MQ-2] CO:     "); Serial.print(coSmooth, 2);    Serial.println(" PPM");
    Serial.print("  [MQ-2] eCO2:   "); Serial.print(eco2Smooth, 2);  Serial.println(" PPM");
    Serial.println("─────────────────────────────────");
}

// Calibrates Ro in clean air. Run once after a full warmup.

float calibrateRo() {
    Serial.println("  [MQ-2] Calibrating — keep sensor in clean air...");
    float rsSum = 0;
    for (int i = 0; i < 100; i++) {
        rsSum += readRsOversampled();
        if (i % 10 == 0) {
            Serial.print("    Progress: ");
            Serial.print(i);
            Serial.println("%");
        }
        delay(50);
    }
    // Ro = Rs_clean_air / RO_CLEAN_AIR_RATIO (9.83 from datasheet)
    float ro = (rsSum / 100.0f) / RO_CLEAN_AIR;
    Serial.print("  [MQ-2] ✔ Calibration done. Ro = ");
    Serial.print(ro, 4);
    Serial.println(" kΩ");
    Serial.println("  [MQ-2]   → Hardcode this into RO_CLEAN_AIR to skip warmup next boot.");
    return ro;
}

// ============================================================
// KY-038 Noise Sensor Functions
// ============================================================
void readNoise() {
    unsigned long startMillis = millis();
    unsigned int peakToPeak = 0;
    unsigned int signalMax = 0;
    unsigned int signalMin = 4095;

    while (millis() - startMillis < sampleWindow) {
        unsigned int sample = analogRead(SOUND_PIN);
        if (sample < 4095) {
            if (sample > signalMax) {
                signalMax = sample;  
            }
            if (sample < signalMin) {
                signalMin = sample;  
            }
        }
    }
    
    peakToPeak = signalMax - signalMin;  
    float targetDb = map(peakToPeak, 0, 2000, 30, 100);
    
    if (targetDb < 30) targetDb = 30;
    if (targetDb > 100) targetDb = 100;
    
    smoothedDb = (0.05 * targetDb) + (0.95 * smoothedDb);
}

void sendNoiseToFirebase(int dbValue) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("  [WiFi] ✗ DISCONNECTED! Cannot push noise data.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(5000);

    String noiseUrl = String(FIREBASE_URL) + "/sensors/noise.json";
    String noiseJson = "{";
    noiseJson += "\"current_db\":" + String(dbValue) + ",";
    noiseJson += "\"last_updated\":{\".sv\":\"timestamp\"}";
    noiseJson += "}";

    http.begin(client, noiseUrl);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.PATCH(noiseJson);

    if (httpCode > 0) {
        Serial.print("  [WiFi] ✓ Noise updated: ");
        Serial.print(dbValue);
        Serial.println(" dB");
    } else {
        Serial.print("  [WiFi] ✗ Noise update FAILED: ");
        Serial.println(http.errorToString(httpCode));
    }
    http.end();

    delay(100);

    String noiseReadUrl = String(FIREBASE_URL) + "/sensors/noise/readings.json";
    String noiseReadJson = "{\"db\":" + String(dbValue) + ",\"timestamp\":{\".sv\":\"timestamp\"}}";

    http.begin(client, noiseReadUrl);
    http.addHeader("Content-Type", "application/json");
    httpCode = http.POST(noiseReadJson);

    if (httpCode > 0) {
        Serial.println("  [WiFi] ✓ Noise reading pushed to chart history");
    } else {
        Serial.print("  [WiFi] ✗ Noise history FAILED: ");
        Serial.println(http.errorToString(httpCode));
    }
    http.end();
}

// ============================================================
// Firebase — Gas
// ============================================================
void sendGasToFirebase(float coValue) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("  [WiFi] ✗ DISCONNECTED! Cannot push CO2.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(5000);

    String co2Url  = String(FIREBASE_URL) + "/sensors/co2.json";
    String co2Json = "{";
    co2Json += "\"current_ppm\":" + String((int)coValue) + ",";
    co2Json += "\"last_updated\":{\".sv\":\"timestamp\"}";
    co2Json += "}";

    http.begin(client, co2Url);
    http.addHeader("Content-Type", "application/json");
    int code = http.PATCH(co2Json);

    if (code > 0) {
        Serial.print("  [WiFi] ✓ CO2 updated: ");
        Serial.print((int)coValue);
        Serial.println(" PPM");
    } else {
        Serial.print("  [WiFi] ✗ CO2 update FAILED: ");
        Serial.println(http.errorToString(code));
    }
    http.end();

    delay(100);

    String co2ReadUrl  = String(FIREBASE_URL) + "/sensors/co2/readings.json";
    String co2ReadJson = "{\"ppm\":" + String((int)coValue) + ",\"timestamp\":{\".sv\":\"timestamp\"}}";

    http.begin(client, co2ReadUrl);
    http.addHeader("Content-Type", "application/json");
    int readCode = http.POST(co2ReadJson);

    if (readCode > 0) {
        Serial.println("  [WiFi] ✓ CO2 reading pushed to chart history");
    } else {
        Serial.print("  [WiFi] ✗ CO2 history FAILED: ");
        Serial.println(http.errorToString(readCode));
    }
    http.end();
}

// ============================================================
// Firebase — IR Counter
// ============================================================
void sendFirebaseData(String type) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("  [WiFi] ✗ DISCONNECTED! Cannot push counter data.");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(5000);

    int snapCount = currentCount;
    int snapIn    = totalIn;
    int snapOut   = totalOut;

    Serial.print("  [WiFi] Preparing to push event: ");
    Serial.println(type);

    String counterUrl  = String(FIREBASE_URL) + "/crowd_counter.json";
    String counterJson = "{";
    counterJson += "\"current_count\":" + String(snapCount) + ",";
    counterJson += "\"total_in\":"      + String(snapIn)    + ",";
    counterJson += "\"total_out\":"     + String(snapOut)   + ",";
    counterJson += "\"last_updated\":{\".sv\":\"timestamp\"}";
    counterJson += "}";

    http.begin(client, counterUrl);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.PATCH(counterJson);

    if (httpCode > 0) {
        Serial.print("  [WiFi] ✓ Counter updated (HTTP ");
        Serial.print(httpCode);
        Serial.println(")");
    } else {
        Serial.print("  [WiFi] ✗ Counter update FAILED: ");
        Serial.println(http.errorToString(httpCode));
    }
    http.end();

    String eventUrl  = String(FIREBASE_URL) + "/crowd_counter/events.json";
    String eventJson = "{\"type\":\"" + type + "\",\"timestamp\":{\".sv\":\"timestamp\"}}";

    http.begin(client, eventUrl);
    http.addHeader("Content-Type", "application/json");
    httpCode = http.POST(eventJson);

    if (httpCode > 0) {
        Serial.print("  [WiFi] ✓ Event logged (HTTP ");
        Serial.print(httpCode);
        Serial.println(")");
    } else {
        Serial.print("  [WiFi] ✗ Event log FAILED: ");
        Serial.println(http.errorToString(httpCode));
    }
    http.end();
}
void showMessage(String msg, int textSize = 2) {
    display.clearDisplay();

    display.setTextSize(textSize);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);

    display.println(msg);

    display.display();
}

// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("Crowd Counter — Dual-Core Architecture");
    Serial.println("========================================");

    pinMode(LEFT_SENSOR_PIN,  INPUT_PULLUP);
    pinMode(RIGHT_SENSOR_PIN, INPUT_PULLUP);

    setupO_Leds();

    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    showMessage("Connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    showMessage("PreHeating");
    Serial.println("========================================");
    Serial.println("Preheating MQ-2 sensor (30s)...");
    for (int i = 30; i > 0; i--) {
        Serial.print("  ");
        Serial.print(i);
        Serial.println("s remaining...");
        delay(1000);
    }
    showMessage("Calibrating");
    Serial.println("✔ Preheat done.");
    Ro = calibrateRo();
    Serial.println("========================================");
    Serial.println();

    Serial.println("Waiting for beam crossings...");
    Serial.println("  LEFT→RIGHT = Person IN");
    Serial.println("  RIGHT→LEFT = Person OUT");
    Serial.println("----------------------------------------");

    delay(500);

    xTaskCreatePinnedToCore(
        sensorTaskcode,
        "SensorTask",
        4096,
        NULL,
        1,
        &SensorTask,
        0);
}
// ============================================================
// Leds and Oled update
// ============================================================
void updateLeds(int current) {
    if (current < 20) {
        // Under capacity — both LEDs solid ON
        digitalWrite(LEFT_LED_PIN, HIGH);
        digitalWrite(RIGHT_LED_PIN, HIGH);
    } else {
        // Over capacity — slow visible blink (1s on, 1s off)
        bool ledState = (millis() / 1000) % 2;
        digitalWrite(LEFT_LED_PIN, ledState);
        digitalWrite(RIGHT_LED_PIN, ledState);
    }
}

void updateOLED(int current) {
    // Only redraw if the count actually changed
    if (current == lastDisplayedCount) return;
    lastDisplayedCount = current;

    Serial.print("  [OLED] Updating display, count = ");
    Serial.println(current);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    if (current < 20) {
        display.setTextSize(3);
        display.setCursor(0, 20);
        display.print(current);
    } else {
        display.setTextSize(2);
        display.setCursor(10, 10);
        display.println("WARNING!");
        display.setCursor(10, 35);
        display.print("Full: ");
        display.print(current);
    }

    display.display();
}


// ============================================================
// Debounced Sensor Read
// ============================================================
int debounce(int pin, int &lastRaw, int &stableReading, unsigned long &lastChange) {
    int raw = digitalRead(pin);
    if (raw != lastRaw) {
        lastChange = millis();
        lastRaw = raw;
    }
    if ((millis() - lastChange) >= DEBOUNCE_MS) {
        stableReading = raw;
    }
    return stableReading;
}

// ============================================================
// Sensor Task (Core 0)
// ============================================================
void sensorTaskcode(void * parameter) {
    Serial.println("  [Core 0] IR Sensor Task running at high speed.");
    for (;;) {
        unsigned long now = millis();

        int L;
        int R;
        if (leftMode){
            L = debounce(LEFT_SENSOR_PIN,  lastRawLeft,  leftReading,  lastLeftChange);
            R = debounce(RIGHT_SENSOR_PIN, lastRawRight, rightReading, lastRightChange);

        } else{
            R = debounce(LEFT_SENSOR_PIN,  lastRawLeft,  leftReading,  lastLeftChange);
            L = debounce(RIGHT_SENSOR_PIN, lastRawRight, rightReading, lastRightChange);
        }
        

        if (now - lastDetectionTime < COOLDOWN_MS) {
            vTaskDelay(2 / portTICK_PERIOD_MS);
            continue;
        }

        switch (state) {
            case IDLE:
                if (L == 0 && R == 1) {
                    state = LEFT_FIRST;
                    stateEnteredAt = now;
                    Serial.println("[BEAM] Left broken → tracking IN...");
                }
                else if (R == 0 && L == 1) {
                    state = RIGHT_FIRST;
                    stateEnteredAt = now;
                    Serial.println("[BEAM] Right broken → tracking OUT...");
                }
                break;

            case LEFT_FIRST:
                if (R == 0) {
                    Serial.println("══════════════════════════════════════");
                    Serial.println("  ➡ PERSON ENTERED (LEFT → RIGHT)");
                    totalIn++;
                    currentCount++;
                    Serial.print("  Count: ");
                    Serial.println(currentCount);
                    Serial.println("══════════════════════════════════════");
                    lastDetectionTime = millis();
                    state = IDLE;
                }
                else if (now - stateEnteredAt > CROSSING_TIMEOUT) {
                    Serial.println("[BEAM] Timeout — resetting (LEFT_FIRST)");
                    state = IDLE;
                }
                break;

            case RIGHT_FIRST:
                if (L == 0) {
                    Serial.println("══════════════════════════════════════");
                    Serial.println("  ⬅ PERSON EXITED (RIGHT → LEFT)");
                    totalOut++;
                    if (currentCount > 0) currentCount--;
                    Serial.print("  Count: ");
                    Serial.println(currentCount);
                    Serial.println("══════════════════════════════════════");
                    lastDetectionTime = millis();
                    state = IDLE;
                }
                else if (now - stateEnteredAt > CROSSING_TIMEOUT) {
                    Serial.println("[BEAM] Timeout — resetting (RIGHT_FIRST)");
                    state = IDLE;
                }
                break;
        }
        // Update LEDs from Core 0 so blinking works even during network requests
        updateLeds(currentCount);

        vTaskDelay(2 / portTICK_PERIOD_MS);
    }
}

// ============================================================
// Main Loop — WiFi Task (Core 1)
// ============================================================
void loop() {
    bool didNetworkRequest = false;

    // Always update LEDs (fast, no I2C)
    updateLeds(currentCount);
    // Update OLED only when count changes
    updateOLED(currentCount);

    if (uploadedIn < totalIn) {
        sendFirebaseData("in");
        uploadedIn++;
        didNetworkRequest = true;
    }
    else if (uploadedOut < totalOut) {
        sendFirebaseData("out");
        uploadedOut++;
        didNetworkRequest = true;
    }

    static unsigned long lastGasUpdate = millis();
    if (!didNetworkRequest && (millis() - lastGasUpdate >= 5000)) {
        lastGasUpdate = millis();
        readAllGases();
        sendGasToFirebase(eco2Smooth);
        
        delay(100);
        
        readNoise();
        sendNoiseToFirebase((int)smoothedDb);
        
        didNetworkRequest = true;
    }

    if (didNetworkRequest) {
        delay(200);
    } else {
        delay(20);
    }
}

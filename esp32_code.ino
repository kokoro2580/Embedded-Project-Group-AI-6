// esp32_scale.ino
//
// Flow:
//  1. Boot  -> LCD ready
//  2. BTN1  -> publish {"action":"start"} to MQTT, wait Pi5 "finish"
//  3. Pi5 sends "finish" -> stepper 5s
//  4. LCD "Finish!", back to IDLE

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "HX711.h"

// =====================================================================
// CONFIGURATION
// =====================================================================
const char* WIFI_SSID     = "FroZe";
const char* WIFI_PASSWORD = "smokecrack";
const char* MQTT_BROKER   = "192.168.101.9";
const int   MQTT_PORT     = 1883;

// =====================================================================
// PINS
// =====================================================================
#define BTN1_PIN        34   // Start button (input-only -- needs external 10k pull-up to 3.3V)
int VM = 19;                 // Vibration motor

// =====================================================================
// VIBRATION PWM
// 3 levels: OFF / LOW / HIGH
// Compatible with ESP32 Arduino Core 2.x and 3.x
// =====================================================================
#if !defined(ESP_ARDUINO_VERSION_MAJOR)
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#if ESP_ARDUINO_VERSION_MAJOR < 3
const int VM_PWM_CH   = 0;
#endif
const int VM_PWM_FREQ = 1000;
const int VM_PWM_RES  = 8;     // duty 0-255

const uint8_t VIB_OFF  = 0;
const uint8_t VIB_LOW  = 120;
const uint8_t VIB_HIGH = 255;
#define STEPPER_IN1     33
#define STEPPER_IN2     32
#define STEPPER_IN3     26
#define STEPPER_IN4     25
#define LOADCELL_DOUT   35
#define LOADCELL_CLK    34

// =====================================================================
// STEPPER
// =====================================================================
const unsigned long STEPPER_RUN_MS = 5000;

// =====================================================================
// MQTT TOPICS
// =====================================================================
const char* MQTT_PUB = "scale/shoot";    // ESP32 -> Pi5
const char* MQTT_SUB = "scale/command";  // Pi5   -> ESP32

// =====================================================================
// WEIGHT SENSOR
// Core 0: weightTask reads HX711 (blocking ~100ms/read)
// Core 1: appTask reads sharedWeightKg via mutex
// =====================================================================
float calibration_factor = 34779.00;
#define zero_factor 8535481

HX711             scale;
SemaphoreHandle_t weightMutex;
volatile float    sharedWeightKg = 0.0f;
volatile bool     doTare         = false;

void requestTare() { doTare = true; }

float getWeightKg() {
    float v;
    xSemaphoreTake(weightMutex, portMAX_DELAY);
    v = sharedWeightKg;
    xSemaphoreGive(weightMutex);
    return v;
}

static void waitReady() {
    while (!scale.is_ready()) vTaskDelay(1 / portTICK_PERIOD_MS);
}
static float readKg() {
    waitReady();
    return scale.get_units() * 0.453592f;
}

void weightTask(void* param) {
    float offset = 0.0f;
    for (int i = 0; i < 5; i++) { waitReady(); scale.get_units(); }
    offset = -readKg();

    for (;;) {
        if (doTare) { doTare = false; offset = -readKg(); }
        float kg = readKg() + offset;
        xSemaphoreTake(weightMutex, portMAX_DELAY);
        sharedWeightKg = kg;
        xSemaphoreGive(weightMutex);
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// =====================================================================
// OBJECTS
// =====================================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// =====================================================================
// STATE MACHINE
// =====================================================================
enum State {
    ST_IDLE,        // Waiting for BTN1
    ST_SENT,        // Published to MQTT, waiting for Pi5 "finish"
    ST_SHOOTING,    // Servo 180->90 + stepper
    ST_RESETTING    // Servo 90->180, return to IDLE
};
State state = ST_IDLE;

// =====================================================================
// FLAGS
// =====================================================================
volatile bool btn1Pressed    = false;
bool          finishReceived = false;

// =====================================================================
// ISR
// =====================================================================
void IRAM_ATTR isr_btn1() {
    static unsigned long lastMs = 0;
    if (millis() - lastMs > 200) {
        btn1Pressed = true;
        lastMs = millis();
    }
}

// =====================================================================
// MQTT CALLBACK
// =====================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.println("[MQTT rx] " + msg);
    if (msg == "finish" && state == ST_SENT) {
        finishReceived = true;
    }
}

// =====================================================================
// NETWORK HELPERS
// =====================================================================
void connectWifi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
}

void ensureMqtt() {
    if (mqtt.connected()) return;
    while (!mqtt.connected()) {
        if (mqtt.connect("ESP32_Scale")) {
            mqtt.subscribe(MQTT_SUB);
        } else {
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
}

// =====================================================================
// LCD HELPER
// =====================================================================
void lcdShow(const char* line1, const char* line2) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
}

// =====================================================================
// VIBRATION
// =====================================================================
void setupVibrationPwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(VM, VM_PWM_FREQ, VM_PWM_RES);
    ledcWrite(VM, VIB_OFF);
#else
    ledcSetup(VM_PWM_CH, VM_PWM_FREQ, VM_PWM_RES);
    ledcAttachPin(VM, VM_PWM_CH);
    ledcWrite(VM_PWM_CH, VIB_OFF);
#endif
}

void setVibration(uint8_t level) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(VM, level);
#else
    ledcWrite(VM_PWM_CH, level);
#endif
}

void buzz(uint8_t level, unsigned long ms) {
    setVibration(level);
    vTaskDelay(ms / portTICK_PERIOD_MS);
    setVibration(VIB_OFF);
}

// Backward-compatible helper: defaults to strong vibration
void buzz(unsigned long ms) {
    buzz(VIB_HIGH, ms);
}

// =====================================================================
// STEPPER -- H-Bridge CW for durationMs, keeps MQTT alive
// =====================================================================
void runStepper(unsigned long durationMs) {
    digitalWrite(STEPPER_IN1, LOW);  digitalWrite(STEPPER_IN2, HIGH);
    digitalWrite(STEPPER_IN3, LOW);  digitalWrite(STEPPER_IN4, HIGH);

    unsigned long endTime = millis() + durationMs;
    while (millis() < endTime) { mqtt.loop(); vTaskDelay(10 / portTICK_PERIOD_MS); }

    digitalWrite(STEPPER_IN1, LOW); digitalWrite(STEPPER_IN2, LOW);
    digitalWrite(STEPPER_IN3, LOW); digitalWrite(STEPPER_IN4, LOW);
}

// Forward declaration
void appTask(void* param);

// =====================================================================
// SETUP
// =====================================================================
void setup() {
    Serial.begin(115200);

    // LCD
    Wire.begin();
    lcd.begin(16, 2);
    lcd.backlight();
    lcdShow("Scale System", "Initializing...");

    // Button
    pinMode(BTN1_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(BTN1_PIN), isr_btn1, FALLING);

    // Vibration motor
    pinMode(VM, OUTPUT);
    setupVibrationPwm();

    // Stepper H-Bridge
    pinMode(STEPPER_IN1, OUTPUT); pinMode(STEPPER_IN2, OUTPUT);
    pinMode(STEPPER_IN3, OUTPUT); pinMode(STEPPER_IN4, OUTPUT);
    digitalWrite(STEPPER_IN1, LOW); digitalWrite(STEPPER_IN2, LOW);
    digitalWrite(STEPPER_IN3, LOW); digitalWrite(STEPPER_IN4, LOW);

    // Weight sensor — Core 0
    lcdShow("Scale init...", "Please wait...");
    weightMutex = xSemaphoreCreateMutex();
    scale.begin(LOADCELL_DOUT, LOADCELL_CLK);
    scale.set_scale(calibration_factor);
    scale.set_offset(zero_factor);
    xTaskCreatePinnedToCore(weightTask, "WeightTask", 4096, NULL, 1, NULL, 0);
    delay(800);  // let weightTask do its initial tare

    // Network
    lcdShow("Connecting WiFi", "Please wait...");
    connectWifi();
    lcdShow("MQTT connecting", "Please wait...");
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    ensureMqtt();

    lcdShow("Scale Ready", "BTN1: Start");
    Serial.println("=== Ready. Press BTN1 to start. ===");

    // Launch app task pinned to Core 1
    // WiFi/MQTT stack runs on Core 0 automatically
    xTaskCreatePinnedToCore(
        appTask,   // task function
        "AppTask", // name
        16384,     // stack size
        NULL,      // parameter
        1,         // priority
        NULL,      // handle
        1          // Core 1
    );
}

// =====================================================================
// APP TASK -- pinned to Core 1
// =====================================================================
void appTask(void* param) {
    for (;;) {
        ensureMqtt();
        mqtt.loop();

        switch (state) {

            case ST_IDLE: {
                // Refresh weight display every 300 ms
                static unsigned long lastWMs = 0;
                if (millis() - lastWMs >= 300) {
                    lastWMs = millis();
                    float kg = getWeightKg();
                    lcd.setCursor(0, 0);
                    lcd.print("W:");
                    lcd.print(kg, 2);
                    lcd.print(" kg      ");
                    lcd.setCursor(0, 1);
                    lcd.print("BTN1:Tare B1:Go ");
                }
                // Short press = tare, long press handled via double-press not needed:
                // For simplicity: BTN1 = publish+start, requestTare on boot
                if (btn1Pressed) {
                    btn1Pressed = false;
                    float kg = getWeightKg();
                    buzz(VIB_LOW, 200);
                    String payload = "{\"action\":\"start\",\"weight_kg\":" + String(kg, 2) + "}";
                    mqtt.publish(MQTT_PUB, payload.c_str());
                    Serial.println("[MQTT tx] " + payload);
                    finishReceived = false;
                    lcdShow("Sent!", "Waiting Pi5...");
                    state = ST_SENT;
                }
                break;
            }

            case ST_SENT:
                if (finishReceived) {
                    Serial.println("[Pi5] Finish received.");
                    state = ST_SHOOTING;
                }
                break;

            case ST_SHOOTING:
                lcdShow("Working...", "Dispensing...");
                Serial.println("[STEPPER] Running...");
                runStepper(STEPPER_RUN_MS);
                Serial.println("[STEPPER] Done.");

                state = ST_RESETTING;
                break;

            case ST_RESETTING:
                buzz(VIB_HIGH, 300);
                lcdShow("Finish!", "BTN1: Start");
                Serial.println("=== Cycle complete. ===");
                btn1Pressed = false;
                state = ST_IDLE;
                break;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void loop() {
    // Empty — all logic runs in appTask on Core 1
    vTaskDelete(NULL);
}

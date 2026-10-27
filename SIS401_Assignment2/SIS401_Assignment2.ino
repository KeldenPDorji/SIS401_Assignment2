/*
  SIS401 Programming Assignment 2
  IoT Data Pipeline: Cloud Ingestion, Real-Time Dashboard and Remote Actuation
  Group 6

  Sensors   : DHT22 (temperature + humidity, GPIO4), LDR module (light, GPIO34)
  Actuators : MG90S servo (GPIO18, 5 V from VIN rail), LED (GPIO26, 220 ohm)
  Edge AI   : Edge Impulse classifier (Normal / Hot-Dry / Cold-Humid) on DHT22 data
  Broker    : Mosquitto on the Raspberry Pi; Node-RED forwards to ThingsBoard Cloud

  MQTT topics
    assignment2/sensors/data          -> {"temperature","humidity","light"}   every 5 s
    assignment2/ai/classification     -> {"label","confidence"}               every 5 s
    assignment2/actuator/command      <- {"servo":0-180} and/or {"led":true|false}
    assignment2/actuator/state        -> {"servo","led","source"}  (acknowledgement, retained)
    assignment2/device/status         -> "online" / "offline"      (retained, last will)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <SIS401_Environment_Classifier_inferencing.h>

// ---------- Pins ----------
#define DHT_PIN    4
#define LDR_PIN    34    // ADC1, input only
#define SERVO_PIN  18
#define LED_PIN    26
#define DHT_TYPE   DHT22

// ---------- Network ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER   = "192.168.X.X";   // Raspberry Pi IP (hostname -I on the Pi)

// ---------- MQTT ----------
const int   MQTT_PORT = 1883;
const char* CLIENT_ID = "SIS401_A2_Group6";

const char* TOPIC_SENSORS = "assignment2/sensors/data";
const char* TOPIC_AI      = "assignment2/ai/classification";
const char* TOPIC_COMMAND = "assignment2/actuator/command";
const char* TOPIC_STATE   = "assignment2/actuator/state";
const char* TOPIC_STATUS  = "assignment2/device/status";

// ---------- Timing ----------
const unsigned long PUBLISH_INTERVAL = 5000;   // sensor + AI cycle
const unsigned long MQTT_RETRY       = 5000;   // reconnect attempt spacing
unsigned long lastPublish = 0;
unsigned long lastMqttTry = 0;

// ---------- Light sensor ----------
const bool LDR_INVERTED = true;   // module output falls as light rises
const int  LDR_SAMPLES  = 8;      // averaged to smooth ADC noise

// ---------- State ----------
struct Readings {
  float temperature = NAN;
  float humidity    = NAN;
  int   light       = 0;
} readings;

struct Prediction {
  String label      = "unknown";
  float  confidence = 0.0f;
  bool   valid      = false;
} prediction;

int  servoAngle = 90;
bool ledOn      = false;

DHT          dht(DHT_PIN, DHT_TYPE);
Servo        servo;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// =====================================================================
//  Actuators
// =====================================================================

// Publishes the current actuator state so the dashboard can confirm
// that a command was actually carried out (retained for late subscribers).
void publishActuatorState(const char* source) {
  JsonDocument doc;
  doc["servo"]  = servoAngle;
  doc["led"]    = ledOn;
  doc["source"] = source;

  char buf[96];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_STATE, buf, true);
}

// Moves the servo in small steps instead of one jump, which keeps the
// motor's start-up current low enough not to brown out the USB supply.
void applyServo(int angle) {
  int target = constrain(angle, 0, 180);   // never drive past the mechanical limits
  int step   = (target > servoAngle) ? 1 : -1;
  while (servoAngle != target) {
    servoAngle += step;
    servo.write(servoAngle);
    delay(8);                               // ~1.5 s for a full 180 degree sweep
  }
}

void applyLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
}

// Command payload is JSON, e.g. {"servo":120} or {"led":true} or both.
void handleCommand(const String& msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg)) {
    Serial.printf("[%7.1fs] CMD rejected, not valid JSON: %s\n", millis() / 1000.0, msg.c_str());
    return;
  }

  bool changed = false;
  if (doc["servo"].is<int>())  { applyServo(doc["servo"].as<int>());  changed = true; }
  if (doc["led"].is<bool>())   { applyLed(doc["led"].as<bool>());     changed = true; }

  if (!changed) {
    Serial.printf("[%7.1fs] CMD ignored, no servo/led field: %s\n", millis() / 1000.0, msg.c_str());
    return;
  }

  publishActuatorState("command");
  Serial.printf("[%7.1fs] CMD %s -> servo=%d led=%s (ack sent)\n",
                millis() / 1000.0, msg.c_str(), servoAngle, ledOn ? "ON" : "OFF");
}

// Runs inside mqtt.loop() whenever a subscribed message arrives.
void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  if (strcmp(topic, TOPIC_COMMAND) == 0) handleCommand(msg);
}

// =====================================================================
//  Connectivity
// =====================================================================

void connectWiFi() {
  Serial.print("WiFi: connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected, IP " + WiFi.localIP().toString());
}

// Non-blocking: tries at most once every 5 s so sensing never stalls.
// The last-will message marks the device offline if it drops unexpectedly.
void ensureMqtt() {
  if (mqtt.connected()) return;
  if (lastMqttTry != 0 && millis() - lastMqttTry < MQTT_RETRY) return;
  lastMqttTry = millis();

  if (mqtt.connect(CLIENT_ID, TOPIC_STATUS, 0, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_COMMAND);
    publishActuatorState("boot");
    Serial.printf("MQTT: connected to %s, subscribed to %s\n", MQTT_BROKER, TOPIC_COMMAND);
  } else {
    Serial.printf("MQTT: failed, rc=%d (retrying in 5 s)\n", mqtt.state());
  }
}

// =====================================================================
//  Sensing
// =====================================================================

// Keeps the last good values if the DHT22 misses a read.
void readDht() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("DHT22: read failed, keeping last values");
    return;
  }
  readings.temperature = t;
  readings.humidity    = h;
}

void readLight() {
  long sum = 0;
  for (int i = 0; i < LDR_SAMPLES; i++) sum += analogRead(LDR_PIN);
  int pct = map(sum / LDR_SAMPLES, 0, 4095, 0, 100);
  readings.light = LDR_INVERTED ? 100 - pct : pct;
}

// =====================================================================
//  Edge AI inference
// =====================================================================

// Normalises the DHT22 values to the 0-1 training range
// (temperature / 50, humidity / 100) and runs the Edge Impulse model.
void runInference() {
  prediction.valid = false;
  if (isnan(readings.temperature) || isnan(readings.humidity)) return;

  float input[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = {
    readings.temperature / 50.0f,
    readings.humidity    / 100.0f
  };

  signal_t signal;
  numpy::signal_from_buffer(input, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("AI: inference error %d\n", err);
    return;
  }

  // Pick the class with the highest probability.
  int best = 0;
  for (int i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > result.classification[best].value) best = i;
  }

  prediction.label      = result.classification[best].label;
  prediction.confidence = result.classification[best].value;
  prediction.valid      = true;
}

// =====================================================================
//  Publishing
// =====================================================================

void publishSensors() {
  if (isnan(readings.temperature)) return;   // nothing valid to send yet

  JsonDocument doc;
  doc["temperature"] = serialized(String(readings.temperature, 1));
  doc["humidity"]    = serialized(String(readings.humidity, 1));
  doc["light"]       = readings.light;

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_SENSORS, buf);
}

void publishPrediction() {
  if (!prediction.valid) return;

  JsonDocument doc;
  doc["label"]      = prediction.label;
  doc["confidence"] = serialized(String(prediction.confidence, 2));

  char buf[96];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_AI, buf);
}

void printCycle() {
  Serial.printf("[%7.1fs] T=%.1fC H=%.1f%% L=%d%% | AI=%s (%.2f) | servo=%d led=%s | %s\n",
                millis() / 1000.0,
                readings.temperature, readings.humidity, readings.light,
                prediction.label.c_str(), prediction.confidence,
                servoAngle, ledOn ? "ON" : "OFF",
                mqtt.connected() ? "published" : "MQTT down");
}

// =====================================================================
//  Main
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SIS401 Assignment 2 : IoT data pipeline ===");

  pinMode(LED_PIN, OUTPUT);
  applyLed(false);

  analogReadResolution(12);
  dht.begin();

  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(servoAngle);                  // start centred

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);
  ensureMqtt();
}

// No delay() here: mqtt.loop() must run continuously so dashboard
// commands reach the actuators immediately, not once every 5 s.
void loop() {
  ensureMqtt();
  mqtt.loop();

  if (millis() - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = millis();
    readDht();
    readLight();
    runInference();
    if (mqtt.connected()) {
      publishSensors();
      publishPrediction();
    }
    printCycle();
  }
}

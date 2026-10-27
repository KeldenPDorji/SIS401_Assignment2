# SIS401 Assignment 2 - IoT Data Pipeline

**Cloud ingestion · real-time dashboard · remote actuation · Edge AI**

**Group 6** · 02230285 · 02230281 · 02230299 · 02230300
**Module:** SIS401 Smart Integrated Systems

An ESP32 reads a DHT22 and an LDR, runs an Edge Impulse classifier on the
readings, and publishes everything over MQTT to Mosquitto on a Raspberry Pi.
Node-RED on the Pi shows the data on a live dashboard, forwards it to
ThingsBoard Cloud, where an alarm rule watches the temperature, and sends servo
and LED commands back to the ESP32, which confirms each one on an
acknowledgement topic.

<p align="center">
  <img src="dashboard_screenshot.png" width="760" alt="Node-RED dashboard">
</p>

---

## Contents

1. [Requirements coverage](#requirements-coverage)
2. [System architecture](#system-architecture)
3. [Hardware](#hardware)
4. [MQTT design](#mqtt-design)
5. [Cloud platform: ThingsBoard](#cloud-platform-thingsboard)
6. [Edge AI extension](#edge-ai-extension)
7. [Extensions beyond Practicals 5 and 6](#extensions-beyond-practicals-5-and-6)
8. [Setup](#setup)
9. [Repository contents](#repository-contents)

---

## Requirements coverage

| Requirement | How it is met | Evidence |
|---|---|---|
| **FR1** Two sensors → MQTT | DHT22 (digital) and LDR (analog) read together, published as JSON every 5 s under `assignment2/…` | [Serial monitor](docs/a2_serial_monitor.png), [MQTT subscriber](docs/a2_mqtt_subscriber.png) |
| **FR2** Cloud platform | Node-RED forwards telemetry to ThingsBoard Cloud with a device access token; **High Temperature** alarm rule | [Telemetry](cloud_screenshot.png), [Alarm](docs/a2_tb_alarm.png) |
| **FR3** Real-time dashboard | Node-RED dashboard: three gauges and a live line chart | [Dashboard](dashboard_screenshot.png) |
| **FR4** Remote actuation | Servo slider and LED switch → `actuator/command`; state confirmed on `actuator/state` | [Hardware photos](#hardware), [MQTT subscriber](docs/a2_mqtt_subscriber.png) |
| **Extension** Edge AI | Edge Impulse model runs on the ESP32; label published to MQTT and shown on the dashboard and in the cloud | [Edge AI](#edge-ai-extension) |

---

## System architecture

```
 DHT22 ─┐                                       ┌──► Node-RED dashboard (/ui)
 LDR  ──┼─► ESP32 ──MQTT──► Mosquitto (Pi) ─────┤
        │     │  Edge AI                        └──► ThingsBoard Cloud
 Servo ◄┤     │  (Edge Impulse)                        telemetry + alarm rule
 LED   ◄┘     └──◄── assignment2/actuator/command ◄── dashboard slider / switch
```

| Part | Choice |
|---|---|
| Microcontroller | ESP32 DevKit |
| Sensors | DHT22 (temperature, humidity) - digital · LDR module (light) - analog |
| Actuators | MG90S servo · LED |
| Broker | Mosquitto 2.0 on Raspberry Pi 4 |
| Dashboard | Node-RED 5 + node-red-dashboard |
| Cloud | ThingsBoard Cloud over MQTT |
| Edge AI | Edge Impulse neural network on the ESP32 |

The ESP32 only ever talks to the local broker. Node-RED acts as the gateway to
the cloud, so the device holds no cloud credentials and the cloud side can
change without reflashing it.

<p align="center">
  <img src="docs/a2_nodered_flow.png" width="760" alt="Node-RED flow">
</p>

---

## Hardware

| Component pin | Connected to | Notes |
|---|---|---|
| DHT22 VCC | ESP32 3.3 V | |
| DHT22 DATA | ESP32 GPIO 4 | 10 kΩ pull-up to 3.3 V |
| DHT22 GND | ESP32 GND | |
| LDR VCC | ESP32 3.3 V | |
| LDR AO | ESP32 GPIO 34 | ADC input only; DO not used |
| LDR GND | ESP32 GND | |
| Servo signal (orange) | ESP32 GPIO 18 | PWM |
| Servo VCC (red) | ESP32 VIN (5 V) | |
| Servo GND (brown) | ESP32 GND | common ground |
| LED anode (long leg) | ESP32 GPIO 26 | through 220 Ω |
| LED cathode | ESP32 GND | |

> **Power note:** run the Raspberry Pi from its own 5 V 3 A charger, not the
> same laptop or USB hub as the ESP32. During testing the servo's start-up
> current on a shared supply reset the Pi.

<table>
  <tr>
    <th>LED off</th>
    <th>LED on - switched from the dashboard</th>
  </tr>
  <tr>
    <td><img src="docs/a2_hardware_led_off.jpeg" width="380" alt="Circuit with LED off"></td>
    <td><img src="docs/a2_hardware_led_on.jpeg" width="380" alt="Circuit with LED on"></td>
  </tr>
</table>

---

## MQTT design

| Topic | Direction | Example payload |
|---|---|---|
| `assignment2/sensors/data` | ESP32 → broker, every 5 s | `{"temperature":28.4,"humidity":44.6,"light":22}` |
| `assignment2/ai/classification` | ESP32 → broker, every 5 s | `{"label":"Normal","confidence":0.95}` |
| `assignment2/actuator/command` | dashboard → ESP32 | `{"servo":45}` or `{"led":false}` |
| `assignment2/actuator/state` | ESP32 → broker, retained | `{"servo":45,"led":false,"source":"command"}` |
| `assignment2/device/status` | ESP32 → broker, retained, last will | `online` / `offline` |

- **Commands and acknowledgements use separate topics.** The ESP32 publishes
  to `actuator/state` only after the actuator has moved, so the dashboard's
  *Confirmed state* shows what the hardware did, not just what was requested.
- **Last will:** if the ESP32 drops off the network, the broker publishes
  `offline` on its behalf.

The subscriber output below shows the whole loop: sensor and AI data arriving,
commands from the dashboard, and an acknowledgement after each one.

<p align="center">
  <img src="docs/a2_mqtt_subscriber.png" width="760" alt="mosquitto_sub output">
</p>

<p align="center">
  <img src="docs/a2_serial_monitor.png" width="760" alt="ESP32 serial monitor">
</p>

---

## Cloud platform: ThingsBoard

- Device **`SIS401_ESP32`**, authenticated with its MQTT **access token**.
- Node-RED publishes to `v1/devices/me/telemetry` on `mqtt.thingsboard.cloud`.
- Telemetry keys: `temperature`, `humidity`, `light`, `ai_label`,
  `ai_confidence`, `servo_angle`, `led_on`.
- **Alarm rule - High Temperature:** severity **Critical** when
  `temperature > 30`, cleared when `temperature <= 30`.

<p align="center">
  <img src="cloud_screenshot.png" width="760" alt="ThingsBoard latest telemetry">
</p>

<p align="center">
  <img src="docs/a2_tb_alarm.png" width="760" alt="ThingsBoard High Temperature alarm">
</p>

---

## Edge AI extension

| Item | Detail |
|---|---|
| Model type | Fully connected neural network (2 inputs → 20 → 10 → 3 classes), quantized int8 |
| Platform | Edge Impulse → Arduino library (TensorFlow Lite for Microcontrollers, EON Compiler) |
| Classes | Cold-Humid · Hot-Dry · Normal |
| Inputs | temperature / 50 and humidity / 100 from the DHT22 |
| Training data | 900 labelled samples (300 per class) generated across the temperature and humidity range of each class, replacing the 30-sample dataset in the shared tutor project |
| Accuracy | **97.2 %** on the validation set (the original 30-sample model: 40 %) |
| On-device cost | ~1 ms per inference, 1.4 KB RAM |

The label and confidence are published to `assignment2/ai/classification`,
shown on the dashboard as **Edge AI condition**, and forwarded to ThingsBoard
as `ai_label` and `ai_confidence`.

---

## Extensions beyond Practicals 5 and 6

- **JSON payloads** in both directions, parsed with ArduinoJson; malformed
  commands are rejected and logged.
- **Acknowledgement topic**, retained so a dashboard opened later still shows
  the current state.
- **Last-will device status** shown on the dashboard.
- **Two actuators** on one command topic, each field optional.
- **Non-blocking reconnection:** MQTT is retried every 5 s without stalling sensing.
- **Gradual servo movement** in 1° steps to limit the motor's start-up current.
- **Gateway-side cloud forwarding** in Node-RED, mapping local topics to flat
  ThingsBoard keys.
- **Edge AI** classification published and displayed alongside the sensor data.

---

## Setup

WiFi details, the broker IP and the ThingsBoard access token are **not
committed**. Fill them in during the steps below.

<details>
<summary><b>1. Raspberry Pi - Mosquitto and Node-RED</b></summary>

```bash
sudo apt install -y mosquitto mosquitto-clients
printf 'listener 1883\nallow_anonymous true\n' | sudo tee /etc/mosquitto/conf.d/lan.conf
sudo systemctl enable --now mosquitto
bash <(curl -sL https://raw.githubusercontent.com/node-red/linux-installers/master/deb/update-nodejs-and-nodered) --confirm-install --confirm-pi --no-init
sudo systemctl enable --now nodered.service
cd ~/.node-red && npm install node-red-dashboard && sudo systemctl restart nodered.service
hostname -I
```

On an iPhone hotspot, turn on **Maximize Compatibility** so both the Pi and
the ESP32 can join (2.4 GHz, WPA2).
</details>

<details>
<summary><b>2. ThingsBoard Cloud</b></summary>

1. Create a device (here `SIS401_ESP32`) and copy its **access token**.
2. On the device, open **Alarm rules** → add `High Temperature`:
   - Argument `temperature` (latest telemetry)
   - Trigger: **Critical** when `greater than 30`
   - Clear: `less or equal 30`
</details>

<details>
<summary><b>3. Node-RED flow</b></summary>

1. Open `http://<pi-ip>:1880` → ☰ → **Import** → `node-red/a2_flow.json`.
2. Double-click **ThingsBoard telemetry** → edit the **ThingsBoard Cloud**
   broker → **Security** → paste the access token as **Username** (leave the
   password empty).
3. **Deploy**. The dashboard is at `http://<pi-ip>:1880/ui`.
</details>

<details>
<summary><b>4. ESP32 firmware</b></summary>

1. Arduino IDE → Library Manager: DHT sensor library, Adafruit Unified Sensor,
   PubSubClient, ArduinoJson (v7), ESP32Servo.
2. Sketch → Include Library → Add .ZIP Library →
   `SIS401_Environment_Classifier_inferencing.zip`.
3. In `SIS401_Assignment2.ino`, set `WIFI_SSID`, `WIFI_PASSWORD` and
   `MQTT_BROKER` (the Pi's IP).
4. Board **ESP32 Dev Module**, upload, and open the Serial Monitor at 115200.
</details>

<details>
<summary><b>5. Verify</b></summary>

```bash
mosquitto_sub -h <pi-ip> -t 'assignment2/#' -v
```

Moving the dashboard slider or switch produces a command and, a moment later,
the matching acknowledgement on `assignment2/actuator/state`.
</details>


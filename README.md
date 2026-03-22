# 🚑 Rakshak 2.0 — AI-Based Smart Emergency Traffic System

> *Saving Lives, One Signal at a Time*

[![Arduino](https://img.shields.io/badge/Arduino-ESP32-blue)](https://www.arduino.cc/)
[![Python](https://img.shields.io/badge/Python-3.10+-green)](https://python.org)
[![YOLOv8](https://img.shields.io/badge/YOLOv8-97%25_mAP-red)](https://ultralytics.com)
[![MQTT](https://img.shields.io/badge/MQTT-HiveMQ-orange)](https://hivemq.com)
[![Firebase](https://img.shields.io/badge/Firebase-Realtime_DB-yellow)](https://firebase.google.com)
[![Open In Colab](https://colab.research.google.com/assets/colab-badge.svg)](https://colab.research.google.com/github/YOUR_USERNAME/YOUR_REPO/blob/main/training/Rakshak2_Ambulance_Training_v2.ipynb)

---

## 👤 Team

**Dhanush S**  
School of Computer Science and Engineering  
**Samved Hackathon**

---

## 📌 Problem Statement

In India, ambulances lose critical minutes at traffic signals. Existing systems rely on GPS pre-emption or manual override — both are expensive, slow, or require infrastructure changes. Rakshak 2.0 solves this using affordable IoT hardware and AI, deployable at any intersection.

---

## 💡 Solution

Rakshak 2.0 automatically detects an approaching ambulance using **two independent AI methods** simultaneously:

1. **Audio AI** — FFT analysis on the INMP441 mic detects the ambulance siren's unique frequency sweep pattern (500–2000 Hz modulation). Rejects claps, horns, and traffic noise.
2. **Visual AI** — YOLOv8 model (trained at 97% accuracy) detects the ambulance in the ESP32-CAM stream.

Only when **both** are confirmed does the system trigger — eliminating false positives almost entirely.

```
Siren detected  +  Ambulance in camera
        ↓
  Signal 2 → GREEN  (ambulance lane cleared)
  All others → RED
        ↓
  Normal cycle resumes automatically after ambulance passes
```

---

## 🏆 Key Results

| Metric | Value |
|--------|-------|
| YOLO mAP@50 | **97%** |
| YOLO Precision | 94% |
| YOLO Recall | 95% |
| End-to-end response time | < 500ms |
| Training dataset size | 1,500+ ambulance images |
| False positive rate | Very low (dual confirmation) |

---

## 🗂️ Project Structure

```
Rakshak2.0/
│
├── arduino/
│   ├── traffic_unit/
│   │   └── traffic_unit.ino      ← ESP32: FFT siren + 4 signal control
│   ├── esp32cam/
│   │   └── esp32cam.ino          ← ESP32-CAM: MJPEG stream server
│   ├── ambulance_unit/
│   │   └── ambulance_unit.ino    ← ESP32: ultrasonic + GPS + alerts
│   └── car_unit/
│       └── car_unit.ino          ← ESP32: OLED display + buzzer
│
├── python/
│   ├── main.py                   ← Main orchestrator (run this)
│   ├── ambulance_detector.py     ← YOLOv8 camera detection
│   ├── test_model.py             ← Quick model test
│   ├── tune_thresholds.py        ← Siren calibration tool
│   └── requirements.txt
│
├── dashboards/
│   ├── hospital_dashboard.html   ← Hospital command center (Firebase live)
│   └── car_dashboard.html        ← Driver alert dashboard (MQTT live)
│
├── training/
│   └── Rakshak2_Ambulance_Training_v2.ipynb  ← Google Colab training
│
└── README.md
```

---

## ⚙️ System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                      RAKSHAK 2.0                                 │
│                                                                  │
│  INMP441 Mic                                                     │
│      ↓ I2S                                                       │
│  Traffic ESP32 ──── FFT Analysis ──── siren_detected ──→ MQTT   │
│                                                           ↓      │
│  ESP32-CAM ──── MJPEG ──── Laptop (YOLOv8) ─── ambulance_det ──→│
│                                                           ↓      │
│                              AND logic: BOTH confirmed?          │
│                                           ↓ YES                  │
│                              MQTT → traffic/signal               │
│                                    = GREEN_AMBULANCE             │
│                                           ↓                      │
│  Traffic ESP32 ← receives command ← Signal 2 → GREEN            │
│                                                                  │
│  Firebase ←── traffic signal states ──── Hospital Dashboard     │
│  MQTT     ←── car/alert ────────────────  Car Driver Dashboard  │
│                                                                  │
│  Ambulance ESP32:                                                │
│    Ultrasonic → car/alert MQTT                                   │
│    GPS → Firebase live tracking                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 🔧 Hardware Used

| Component | Qty | Purpose |
|-----------|-----|---------|
| ESP32 Dev Board | 3 | Traffic unit + Ambulance + Car unit |
| ESP32-CAM (AI Thinker) | 1 | Live video stream |
| INMP441 I2S Microphone | 1 | FFT siren detection |
| SSD1306 OLED (128×64) | 1 | Car unit alert display |
| Hall Effect Sensors | 2 | Physical backup ambulance detection |
| IR Sensors | 2 | Vehicle presence detection |
| HC-SR04 Ultrasonic | 1 | Ambulance proximity alert |
| Neo-6M GPS Module | 1 | Live ambulance tracking |
| LEDs (Red/Yellow/Green) | 12 | 4 traffic signals × 3 colours |
| Buzzer | 2 | Alert feedback |
| Laptop | 1 | YOLOv8 inference + Python |

---

## 🚀 Quick Start

### 1. Clone

```bash
git clone https://github.com/YOUR_USERNAME/rakshak2.git
cd rakshak2
```

### 2. Python setup

```bash
cd python/
python -m venv venv

# Windows
venv\Scripts\activate

# Mac/Linux
source venv/bin/activate

pip install -r requirements.txt
```

### 3. Get the trained model

Download `ambulance_yolov8s.onnx` from the [Releases](../../releases) page  
and place it in the `python/` folder.

Or train your own — open `training/Rakshak2_Ambulance_Training_v2.ipynb` in Google Colab.

### 4. Add your WiFi credentials

In each `.ino` file, fill in:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
```

Files to update:

- `arduino/traffic_unit/traffic_unit.ino`
- `arduino/esp32cam/esp32cam.ino`
- `arduino/ambulance_unit/ambulance_unit.ino`
- `arduino/car_unit/car_unit.ino`

### 5. Flash ESP32s

| Board | Sketch | Extra libraries needed |
|-------|--------|----------------------|
| Traffic ESP32 | `traffic_unit.ino` | `arduinoFFT` by Enrique Condes |
| ESP32-CAM | `esp32cam.ino` | Board: AI Thinker, Partition: Huge APP |
| Ambulance ESP32 | `ambulance_unit.ino` | `TinyGPS++` by Mikal Hart |
| Car ESP32 | `car_unit.ino` | `Adafruit_SSD1306` |

### 6. Run the system

```bash
# Note your ESP32-CAM IP from Serial Monitor, then:
python main.py --cam-url http://192.168.x.x/ --yolo-model ambulance_yolov8s.onnx
```

### 7. Open dashboards

Open directly in Chrome — no server needed:

- `dashboards/hospital_dashboard.html`
- `dashboards/car_dashboard.html`

---

## 🧠 How Siren Detection Works

The INMP441 mic feeds audio to the ESP32 via I2S. Every 64ms:

```
1. Read 1024 samples at 16kHz
2. Compute RMS — skip if near silence
3. Apply Hann window → run FFT
4. Gate 1: Is 15%+ of energy in 500–2000 Hz band?
5. Gate 2: Does dominant frequency sweep (std dev > 8 Hz)?
           Siren: sweeps 700→1600 Hz repeatedly = HIGH std
           Traffic noise: stays flat = LOW std
6. Both gates pass for 3 consecutive frames?
           → Publish: traffic/ambulance = siren_detected
```

This specifically rejects: engine noise (80–400 Hz), constant tones, claps, music.

---

## 🎯 How to Calibrate Siren Threshold

```bash
# Connect Traffic ESP32 via USB, close Arduino Serial Monitor, then:
python tune_thresholds.py --port COM17          # Windows
python tune_thresholds.py --port /dev/ttyUSB0   # Linux/Mac

# 3 phases — press Enter to advance each:
# Phase 1: QUIET  (30 sec silence near mic)
# Phase 2: SIREN  (play ambulance siren MP3 loud near mic)
# Phase 3: NOISE  (clap, talk, traffic sounds)

# Output: exact #define ENERGY_THRESHOLD value to paste in .ino
```

---

## 📡 MQTT Topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `traffic/ambulance` | Traffic ESP32 → Laptop | `siren_detected` / `siren_cleared` |
| `traffic/signal` | Laptop → Traffic ESP32 | `GREEN_AMBULANCE` / `RESET_NORMAL` |
| `traffic/status` | Traffic ESP32 → Dashboards | `Signal 2 GREEN` |
| `car/alert` | Ambulance ESP32 → Car ESP32 | `AMBULANCE NEAR! MOVE ASIDE` |
| `traffic/camera` | Laptop → Dashboards | `ambulance_detected` |
| `traffic/heartbeat` | Traffic ESP32 → All | `traffic_alive` |

---

## 🔌 Wiring Quick Reference

**INMP441 Mic → Traffic ESP32**

```
VDD → 3.3V    GND → GND
SD  → GPIO 35  SCK → GPIO 2  WS → GPIO 4  L/R → GND
```

**SSD1306 OLED → Car ESP32**

```
VCC → 3.3V    GND → GND    SDA → GPIO 21    SCL → GPIO 22
```

**HC-SR04 → Ambulance ESP32**

```
VCC → 5V    GND → GND    TRIG → GPIO 5    ECHO → GPIO 18
```

---

## 🐛 Common Issues

| Problem | Fix |
|---------|-----|
| Siren not detecting | Run `tune_thresholds.py` to calibrate for your environment |
| Camera not opening | Check ESP32-CAM IP in Serial Monitor, use `/capture` endpoint |
| Signal not turning green | Both siren AND camera must confirm simultaneously |
| ESP32 won't connect to WiFi | Must use 2.4GHz — ESP32 does not support 5GHz |
| Car unit stuck on "Connecting" | Power from phone charger or powerbank, not laptop USB |
| OLED blank | SDA=GPIO21, SCL=GPIO22, power with 3.3V not 5V |
| GPS no fix | Take outdoors — needs clear sky, 2–5 min for first fix |
| Multiple LEDs on in traffic unit | Flash latest `traffic_unit.ino` (fixed pin control) |

---

## 📄 License

This project is submitted for **Samved Hackathon**.  
Open source for educational and research use.

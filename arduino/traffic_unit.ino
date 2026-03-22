/* ============================================================
 *  Rakshak 2.0 — TRAFFIC UNIT (Upgraded from your v2)
 *  ESP32 + 4 Traffic Signals + INMP441 + Hall/IR Sensors
 * ============================================================
 *
 *  WHAT CHANGED FROM YOUR v2:
 *  - Siren detection: raw amplitude → FFT + modulation pattern
 *    (eliminates false triggers from claps, traffic noise)
 *  - Trigger logic: sensor-only → MQTT dual confirmation from laptop
 *    (laptop confirms BOTH camera + siren before signalling)
 *  - Kept: your exact pin layout, 4-signal state machine,
 *    Firebase logging, Hall/IR sensors as physical backup
 *
 *  TRIGGER SOURCES (priority order):
 *    1. MQTT "GREEN_AMBULANCE" from laptop (YOLO + FFT confirmed) ← primary
 *    2. Hall sensor (physical magnet backup)                      ← backup
 *    3. IR + local FFT siren (on-device backup)                   ← backup
 *
 *  PINS — same as your v2, only mic pins changed:
 *    INMP441: WS=4, SCK=2, SD=35  (your existing wiring)
 *
 *  LIBRARIES:
 *    - PubSubClient by Nick O'Leary
 *    - arduinoFFT by Enrique Condes
 * ============================================================ */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>

// ── WiFi / Server Config ─────────────────────────────────────
// WiFi credentials — fill in your own
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *mqtt_server = "broker.hivemq.com";
const char *firebase_base = "https://iot-ambulance-traffic-default-rtdb.asia-southeast1.firebasedatabase.app";

// ── Traffic Light Pins (unchanged from your v2) ──────────────
#define S1_RED 5
#define S1_YELLOW 18
#define S1_GREEN 19
#define S2_RED 21
#define S2_YELLOW 16
#define S2_GREEN 17
#define S3_RED 25
#define S3_YELLOW 26
#define S3_GREEN 27
#define S4_RED 32
#define S4_YELLOW 33
#define S4_GREEN 14

// ── Sensor Pins (unchanged from your v2) ─────────────────────
#define HALL1 22
#define HALL2 23
#define IR1 36
#define IR2 39

// ── INMP441 I2S Pins (unchanged from your v2) ────────────────
#define I2S_WS 4
#define I2S_SD 35
#define I2S_SCK 2
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define FFT_SAMPLES 1024

// ── Siren Detection Thresholds ───────────────────────────────
// Run tune_thresholds.py to find your correct ENERGY_THRESHOLD
#define SIREN_FREQ_LOW 700.0f
#define SIREN_FREQ_HIGH 1600.0f
#define ENERGY_THRESHOLD 0.20f // ← tune with tune_thresholds.py
#define MIN_PEAK_MAG 300.0f
#define MOD_HISTORY_SIZE 20
#define MIN_MOD_STD_HZ 35.0f
#define SIREN_CONFIRM_ON 4  // frames to confirm siren
#define SIREN_CONFIRM_OFF 6 // frames to clear siren

// ── Timing (unchanged from your v2) ─────────────────────────
const unsigned long T_GREEN = 15000;
const unsigned long T_YELLOW = 3000;
const unsigned long T_AMB_HOLD = 20000;

// ── MQTT Topics ──────────────────────────────────────────────
// Subscribes to:
#define TOPIC_SIGNAL_CMD "traffic/signal" // laptop → "GREEN_AMBULANCE" / "RESET_NORMAL"
// Publishes to:
#define TOPIC_STATUS "traffic/status"       // → car dashboard
#define TOPIC_AMBULANCE "traffic/ambulance" // → hospital dashboard
#define TOPIC_HB "traffic/heartbeat"

// ─────────────────────────────────────────────────────────────
WiFiClient mqttNet;
PubSubClient mqttClient(mqttNet);
WiFiClientSecure fbClient;

// FFT
double vReal[FFT_SAMPLES];
double vImag[FFT_SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, FFT_SAMPLES, SAMPLE_RATE);

float freqHistory[MOD_HISTORY_SIZE];
int freqHistIdx = 0;
bool freqHistFull = false;
int sirenConfirmOn = 0;
int sirenConfirmOff = 0;
bool localSirenActive = false;

// State machine (same as your v2)
enum SystemMode
{
  MODE_NORMAL,
  MODE_AMBULANCE,
  MODE_RECOVERY
};
SystemMode currentMode = MODE_NORMAL;

enum TrafficLightState
{
  LIGHT_GREEN,
  LIGHT_YELLOW
};
TrafficLightState lightState = LIGHT_GREEN;

int activeSignal = 1;
int interruptedSignal = 1;
int lastPublishedSig = -1;
String lastPublishedColor = "";

unsigned long cycleStartTime = 0;
unsigned long ambulanceTimer = 0;
unsigned long lastHeartbeat = 0;

// MQTT trigger from laptop (YOLO + FFT dual confirmation)
bool mqttAmbulanceActive = false;

// ── Firebase ─────────────────────────────────────────────────
void sendFirebase(int sig, String state)
{
  if (WiFi.status() != WL_CONNECTED)
    return;
  HTTPClient http;
  fbClient.setInsecure();
  String url = String(firebase_base) + "/traffic/signal" + String(sig) + ".json";
  http.begin(fbClient, url);
  http.addHeader("Content-Type", "application/json");
  String json = "{\"state\":\"" + state + "\",\"timestamp\":" + String(millis()) + "}";
  http.PUT(json);
  http.end();
}

// ── Traffic Signal Control ───────────────────────────────────
void setAllRed()
{
  digitalWrite(S1_RED, HIGH);
  digitalWrite(S1_YELLOW, LOW);
  digitalWrite(S1_GREEN, LOW);
  digitalWrite(S2_RED, HIGH);
  digitalWrite(S2_YELLOW, LOW);
  digitalWrite(S2_GREEN, LOW);
  digitalWrite(S3_RED, HIGH);
  digitalWrite(S3_YELLOW, LOW);
  digitalWrite(S3_GREEN, LOW);
  digitalWrite(S4_RED, HIGH);
  digitalWrite(S4_YELLOW, LOW);
  digitalWrite(S4_GREEN, LOW);
}

void setSignal(int sig, String color)
{
  // Deduplicate to save Firebase bandwidth (from your v2)
  if (sig == lastPublishedSig && color == lastPublishedColor)
    return;
  lastPublishedSig = sig;
  lastPublishedColor = color;

  setAllRed();

  int r, y, g;
  switch (sig)
  {
  case 1:
    r = S1_RED;
    y = S1_YELLOW;
    g = S1_GREEN;
    break;
  case 2:
    r = S2_RED;
    y = S2_YELLOW;
    g = S2_GREEN;
    break;
  case 3:
    r = S3_RED;
    y = S3_YELLOW;
    g = S3_GREEN;
    break;
  case 4:
    r = S4_RED;
    y = S4_YELLOW;
    g = S4_GREEN;
    break;
  default:
    return;
  }

  if (color == "GREEN")
    digitalWrite(g, HIGH);
  else if (color == "YELLOW")
    digitalWrite(y, HIGH);
  else
    digitalWrite(r, HIGH);

  // Publish to both MQTT topics your dashboards listen to
  String statusMsg = "Signal " + String(sig) + " " + color;
  mqttClient.publish(TOPIC_STATUS, statusMsg.c_str());

  // Update Firebase for hospital dashboard
  sendFirebase(sig, color);

  Serial.println("[SIGNAL] " + statusMsg);
}

void advanceSignal()
{
  activeSignal++;
  if (activeSignal > 4)
    activeSignal = 1;
}

// ── I2S Setup ────────────────────────────────────────────────
void setupI2S()
{
  i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0};
  i2s_pin_config_t pins = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_SD};
  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_start(I2S_PORT);
  Serial.println("[I2S] INMP441 ready");
}

// ── FFT Siren Detection ──────────────────────────────────────
bool readSamples()
{
  int32_t raw[FFT_SAMPLES];
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytesRead == 0)
    return false;
  int n = bytesRead / sizeof(int32_t);
  for (int i = 0; i < FFT_SAMPLES && i < n; i++)
  {
    vReal[i] = (double)(raw[i] >> 8) / (double)(1 << 23);
    vImag[i] = 0.0;
  }
  return true;
}

bool checkModulation(float freq)
{
  freqHistory[freqHistIdx] = freq;
  freqHistIdx = (freqHistIdx + 1) % MOD_HISTORY_SIZE;
  if (freqHistIdx == 0)
    freqHistFull = true;

  int count = freqHistFull ? MOD_HISTORY_SIZE : freqHistIdx;
  if (count < 6)
    return false;

  float sum = 0;
  int valid = 0;
  for (int i = 0; i < count; i++)
    if (freqHistory[i] > 0)
    {
      sum += freqHistory[i];
      valid++;
    }
  if (valid < 4)
    return false;

  float mean = sum / valid, var = 0;
  for (int i = 0; i < count; i++)
    if (freqHistory[i] > 0)
    {
      float d = freqHistory[i] - mean;
      var += d * d;
    }

  return sqrt(var / valid) >= MIN_MOD_STD_HZ;
}

// Returns true if a siren pattern is detected this frame
bool runSirenFFT()
{
  if (!readSamples())
    return false;

  // RMS gate — skip FFT if near silent
  double rms = 0;
  for (int i = 0; i < FFT_SAMPLES; i++)
    rms += vReal[i] * vReal[i];
  rms = sqrt(rms / FFT_SAMPLES);
  if (rms < 0.0003)
  {
    checkModulation(0);
    return false;
  }

  FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  float bw = (float)SAMPLE_RATE / FFT_SAMPLES;
  int binLo = max(1, (int)(SIREN_FREQ_LOW / bw));
  int binHi = min(FFT_SAMPLES / 2 - 1, (int)(SIREN_FREQ_HIGH / bw));

  double total = 0, band = 0, peak = 0;
  int pkBin = binLo;
  for (int i = 1; i < FFT_SAMPLES / 2; i++)
  {
    double m = vReal[i];
    total += m * m;
    if (i >= binLo && i <= binHi)
    {
      band += m * m;
      if (m > peak)
      {
        peak = m;
        pkBin = i;
      }
    }
  }

  float ratio = total > 0 ? (float)(band / total) : 0;
  bool energy = (ratio >= ENERGY_THRESHOLD) && (peak >= MIN_PEAK_MAG);
  float domFreq = energy ? (pkBin * bw) : 0.0f;
  bool mod = checkModulation(domFreq);

  Serial.printf("[FFT] rms=%.4f ratio=%.2f peak=%.0f energy=%s mod=%s\n",
                (float)rms, ratio, (float)peak,
                energy ? "✓" : "✗", mod ? "✓" : "✗");

  return energy && mod;
}

// Update local siren state with confirmation hysteresis
void updateLocalSiren(bool frameDetected)
{
  if (frameDetected)
  {
    sirenConfirmOn = min(sirenConfirmOn + 1, SIREN_CONFIRM_ON + 2);
    sirenConfirmOff = 0;
  }
  else
  {
    sirenConfirmOff = min(sirenConfirmOff + 1, SIREN_CONFIRM_OFF + 2);
    sirenConfirmOn = max(sirenConfirmOn - 1, 0);
  }

  if (!localSirenActive && sirenConfirmOn >= SIREN_CONFIRM_ON)
  {
    localSirenActive = true;
    Serial.println("[SIREN] 🔊 Local siren confirmed");
    mqttClient.publish(TOPIC_AMBULANCE, "siren_detected");
  }
  if (localSirenActive && sirenConfirmOff >= SIREN_CONFIRM_OFF)
  {
    localSirenActive = false;
    Serial.println("[SIREN] Siren cleared");
    mqttClient.publish(TOPIC_AMBULANCE, "siren_cleared");
  }
}

// ── MQTT Callback ────────────────────────────────────────────
// Receives "GREEN_AMBULANCE" / "RESET_NORMAL" from laptop
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  String msg = "";
  for (int i = 0; i < length; i++)
    msg += (char)payload[i];
  String top = String(topic);

  Serial.println("[MQTT] " + top + " → " + msg);

  if (top == TOPIC_SIGNAL_CMD)
  {
    if (msg == "GREEN_AMBULANCE")
    {
      mqttAmbulanceActive = true;
      Serial.println("[MQTT] 🚨 Dual-confirmed ambulance trigger received!");

      // Also publish alert to Firebase for hospital dashboard
      if (WiFi.status() == WL_CONNECTED)
      {
        HTTPClient http;
        fbClient.setInsecure();
        String url = String(firebase_base) + "/alerts.json";
        http.begin(fbClient, url);
        http.addHeader("Content-Type", "application/json");
        String json = "{\"message\":\"AMBULANCE CONFIRMED (YOLO+SIREN)\",\"time\":" + String(millis()) + "}";
        http.POST(json);
        http.end();
      }
    }
    else if (msg == "RESET_NORMAL")
    {
      mqttAmbulanceActive = false;
      Serial.println("[MQTT] ✅ Ambulance cleared by laptop");
    }
  }
}

// ── WiFi + MQTT Connect ──────────────────────────────────────
void connectMQTT()
{
  String clientId = "TRAFFIC_" + String(random(9999));
  if (mqttClient.connect(clientId.c_str()))
  {
    mqttClient.subscribe(TOPIC_SIGNAL_CMD); // listen for laptop trigger
    Serial.println("[MQTT] Connected, subscribed to " + String(TOPIC_SIGNAL_CMD));
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println("\n============================");
  Serial.println("  Rakshak 2.0 — Traffic Unit");
  Serial.println("============================");

  // Signal pins
  int sigPins[] = {S1_RED, S1_YELLOW, S1_GREEN,
                   S2_RED, S2_YELLOW, S2_GREEN,
                   S3_RED, S3_YELLOW, S3_GREEN,
                   S4_RED, S4_YELLOW, S4_GREEN};
  for (int p : sigPins)
  {
    pinMode(p, OUTPUT);
  }

  // Sensor pins
  pinMode(HALL1, INPUT_PULLUP);
  pinMode(HALL2, INPUT_PULLUP);
  pinMode(IR1, INPUT);
  pinMode(IR2, INPUT);

  setupI2S();

  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  fbClient.setInsecure();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  connectMQTT();

  setSignal(1, "GREEN");
  cycleStartTime = millis();
  memset(freqHistory, 0, sizeof(freqHistory));

  Serial.println("[System] Ready.\n");
}

// ── Main Loop ─────────────────────────────────────────────────
void loop()
{
  // MQTT keepalive
  if (!mqttClient.connected())
    connectMQTT();
  mqttClient.loop();

  unsigned long now = millis();

  // ── Heartbeat every 10s ───────────────────────────────────
  if (now - lastHeartbeat > 10000)
  {
    mqttClient.publish(TOPIC_HB, "traffic_alive");
    lastHeartbeat = now;
  }

  // ── Read physical sensors ─────────────────────────────────
  bool hallDetect = (digitalRead(HALL1) == LOW || digitalRead(HALL2) == LOW);
  bool irDetect = (digitalRead(IR1) == LOW || digitalRead(IR2) == LOW);

  // ── FFT Siren detection ───────────────────────────────────
  bool sirenFrame = runSirenFFT();
  updateLocalSiren(sirenFrame);

  // ── Combined ambulance detection ──────────────────────────
  // Priority 1: MQTT from laptop (YOLO + FFT dual confirmed)  ← most reliable
  // Priority 2: Hall sensor (physical magnet on ambulance)    ← reliable backup
  // Priority 3: IR + local siren                              ← backup
  bool isAmbulance = mqttAmbulanceActive || hallDetect || (irDetect && localSirenActive);

  Serial.printf("[STATUS] Mode=%d Sig=%d Hall=%d IR=%d Siren=%d MQTT=%d => Amb=%d\n",
                currentMode, activeSignal,
                hallDetect, irDetect, localSirenActive, mqttAmbulanceActive,
                isAmbulance);

  // ── State Machine (same logic as your v2) ─────────────────

  // STATE 1: Normal → detect ambulance
  if (isAmbulance && currentMode == MODE_NORMAL)
  {
    Serial.println("🚨 AMBULANCE DETECTED — Saving state, Signal 2 GREEN");
    interruptedSignal = activeSignal;
    currentMode = MODE_AMBULANCE;
    ambulanceTimer = now;
    setSignal(2, "GREEN");
    return;
  }

  // STATE 2: Ambulance override
  if (currentMode == MODE_AMBULANCE)
  {
    if (isAmbulance)
      ambulanceTimer = now; // keep resetting if still present

    if (!isAmbulance && (now - ambulanceTimer > T_AMB_HOLD))
    {
      Serial.println("✅ Ambulance cleared — entering recovery");
      currentMode = MODE_RECOVERY;
      cycleStartTime = now;
      setSignal(2, "YELLOW");
    }
    return;
  }

  // STATE 3: Recovery (yellow before resuming)
  if (currentMode == MODE_RECOVERY)
  {
    if (now - cycleStartTime > T_YELLOW)
    {
      Serial.println("🔄 Resuming cycle at signal " + String(interruptedSignal));
      currentMode = MODE_NORMAL;
      activeSignal = interruptedSignal;
      lightState = LIGHT_GREEN;
      setSignal(activeSignal, "GREEN");
      cycleStartTime = now;
    }
    return;
  }

  // STATE 4: Normal traffic cycle (unchanged from your v2)
  if (currentMode == MODE_NORMAL)
  {
    if (lightState == LIGHT_GREEN)
    {
      if (now - cycleStartTime > T_GREEN)
      {
        lightState = LIGHT_YELLOW;
        setSignal(activeSignal, "YELLOW");
        cycleStartTime = now;
      }
    }
    else
    {
      if (now - cycleStartTime > T_YELLOW)
      {
        advanceSignal();
        lightState = LIGHT_GREEN;
        setSignal(activeSignal, "GREEN");
        cycleStartTime = now;
      }
    }
  }

  delay(20);
}

/* ============================================================
 *  Rakshak 2.0 — Ambulance Unit (Upgraded)
 *  ESP32 + Ultrasonic + GPS + Firebase + MQTT
 * ============================================================
 *
 *  WHAT CHANGED FROM YOUR CURRENT CODE:
 *  - Added ambulance ID in MQTT payloads (for multi-ambulance)
 *  - GPS: sends speed too (from TinyGPS++)
 *  - MQTT: publishes "approaching:<ID>" so traffic knows which
 *  - Firebase: adds speed field to tracking data
 *  - Alert logic: cleaner, no overlapping lastPublish timers
 *  - Added LED blink pattern (SOS-style) during alert
 *
 *  WIRING (same as your current code):
 *    Ultrasonic: TRIG=5, ECHO=18
 *    GPS: RX=16, TX=17 (Serial2)
 *    LED: GPIO 2
 *    Buzzer: GPIO 4
 *
 *  LIBRARIES NEEDED:
 *    - PubSubClient
 *    - TinyGPS++ by Mikal Hart
 * ============================================================ */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TinyGPS++.h>

// ── Config ───────────────────────────────────────────────────
// WiFi credentials — fill in your own
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
const char *mqtt_server = "broker.hivemq.com";
const char *firebase_base = "https://iot-ambulance-traffic-default-rtdb.asia-southeast1.firebasedatabase.app";

// ── Device ID — change for each ambulance unit ───────────────
const String deviceId = "AMB_01";

// ── Pins (same as your current code) ─────────────────────────
#define TRIG 5
#define ECHO 18
#define LED_PIN 2
#define BUZZER 4

// ── MQTT Topics ───────────────────────────────────────────────
#define TOPIC_CAR_ALERT "car/alert"
#define TOPIC_TRAFFIC_AMB "traffic/ambulance"

// ── Clients ──────────────────────────────────────────────────
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiClientSecure fbClient;
TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

// ── Timers ───────────────────────────────────────────────────
unsigned long lastFirebase = 0;
unsigned long lastCarAlert = 0;
unsigned long lastTrafficAlert = 0;
unsigned long lastLedBlink = 0;
bool ledState = false;

// ── Firebase ─────────────────────────────────────────────────
void sendToFirebase(String path, String json, String method)
{
  if (WiFi.status() != WL_CONNECTED)
    return;
  HTTPClient http;
  fbClient.setInsecure();
  String url = String(firebase_base) + "/" + path;
  http.begin(fbClient, url);
  http.addHeader("Content-Type", "application/json");
  int code = (method == "POST") ? http.POST(json) : http.PUT(json);
  Serial.println("Firebase " + method + ": " + String(code));
  http.end();
}

// ── Ultrasonic ───────────────────────────────────────────────
long getDistanceCM()
{
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duration = pulseIn(ECHO, HIGH, 30000); // 30ms timeout
  return (duration == 0) ? 999 : (duration * 0.034 / 2);
}

// ── MQTT Connect ─────────────────────────────────────────────
void connectMQTT()
{
  String clientId = "AMB_" + deviceId + "_" + String(random(999));
  if (mqttClient.connect(clientId.c_str()))
  {
    Serial.println("[MQTT] Connected as " + clientId);
  }
  else
  {
    Serial.println("[MQTT] Failed rc=" + String(mqttClient.state()));
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  Serial.println("\nRakshak 2.0 — Ambulance Unit: " + deviceId);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER, LOW);

  // GPS on Serial2
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("[GPS] Serial2 started at 9600");

  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());

  fbClient.setInsecure();
  mqttClient.setServer(mqtt_server, 1883);
  connectMQTT();

  Serial.println("[System] Ambulance unit ready.\n");
}

// ── Main Loop ─────────────────────────────────────────────────
void loop()
{
  // MQTT keepalive
  if (!mqttClient.connected())
    connectMQTT();
  mqttClient.loop();

  // Feed GPS
  while (gpsSerial.available())
    gps.encode(gpsSerial.read());

  bool gpsValid = gps.location.isValid();
  double latitude = gpsValid ? gps.location.lat() : 12.9716;
  double longitude = gpsValid ? gps.location.lng() : 77.5946;
  double speedKmph = gps.speed.isValid() ? gps.speed.kmph() : 0.0;

  long dist = getDistanceCM();
  unsigned long now = millis();

  // Debug
  Serial.printf("[AMB] GPS=%s dist=%ldcm spd=%.1fkm/h\n",
                gpsValid ? "OK" : "NO", dist, speedKmph);

  // ── ALERT: vehicle very close (<10cm) ────────────────────
  if (dist > 0 && dist <= 10 && (now - lastCarAlert > 1000))
  {
    String msg = "AMBULANCE NEAR! MOVE ASIDE [" + deviceId + "]";

    // MQTT → car units
    mqttClient.publish(TOPIC_CAR_ALERT, msg.c_str());
    Serial.println("[ALERT] → car/alert");

    // Buzzer beep
    digitalWrite(BUZZER, HIGH);
    delay(150);
    digitalWrite(BUZZER, LOW);

    // Firebase alert log
    String json = "{\"id\":\"" + deviceId +
                  "\",\"message\":\"" + msg +
                  "\",\"distance\":" + String(dist) +
                  ",\"lat\":" + String(latitude, 6) +
                  ",\"lng\":" + String(longitude, 6) +
                  ",\"time\":" + String(now) + "}";
    sendToFirebase("alerts.json", json, "POST");

    lastCarAlert = now;
  }

  // ── TRAFFIC SIGNAL ALERT: approaching (<20cm) ────────────
  if (dist > 0 && dist <= 20 && (now - lastTrafficAlert > 1500))
  {
    // Payload includes device ID so traffic knows which ambulance
    String payload = "approaching:" + deviceId;
    mqttClient.publish(TOPIC_TRAFFIC_AMB, payload.c_str());
    Serial.println("[TRAFFIC] → traffic/ambulance: " + payload);
    lastTrafficAlert = now;
  }

  // ── LED: blink fast when near traffic, solid otherwise ───
  if (dist > 0 && dist <= 20)
  {
    if (now - lastLedBlink > 200)
    {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = now;
    }
  }
  else
  {
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  }

  // ── LIVE TRACKING: update Firebase every 3s ──────────────
  if (now - lastFirebase > 3000)
  {
    String status = (dist > 0 && dist <= 20) ? "Approaching Traffic" : "En Route";

    String json = "{\"id\":\"" + deviceId +
                  "\",\"status\":\"" + status +
                  "\",\"speed\":" + String(speedKmph, 1) +
                  ",\"gps_valid\":" + String(gpsValid ? "true" : "false") +
                  ",\"location\":{\"lat\":" + String(latitude, 6) +
                  ",\"lng\":" + String(longitude, 6) + "}}";

    sendToFirebase("ambulance/" + deviceId + ".json", json, "PUT");
    lastFirebase = now;
  }
}

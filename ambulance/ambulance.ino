#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// --- CONFIG ---
const char* ssid = "Dhanush";
const char* password = "samsungm";
const char* mqtt_server = "broker.hivemq.com";
const char* firebase_base = "https://iot-ambulance-traffic-default-rtdb.asia-southeast1.firebasedatabase.app";

// --- PINS ---
#define TRIG 5
#define ECHO 18
#define LED_PIN 2
#define BUZZER 4

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiClientSecure fbClient; 

unsigned long lastFirebase = 0;
unsigned long lastPublish = 0;
String deviceId = "AMB_01";

// --- FIREBASE HELPER (UPDATED) ---
// Uses POST for alerts (Append) and PUT for status (Overwrite)
void sendToFirebase(String path, String json, String method) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  fbClient.setInsecure();
  
  String url = String(firebase_base) + "/" + path;
  http.begin(fbClient, url);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode;
  if (method == "POST") {
    httpResponseCode = http.POST(json); // APPENDS data (History)
  } else {
    httpResponseCode = http.PUT(json);  // OVERWRITES data (Current Status)
  }
  
  if (httpResponseCode > 0) {
    Serial.println("Firebase " + method + " Success: " + String(httpResponseCode));
  } else {
    Serial.println("Firebase Error: " + http.errorToString(httpResponseCode));
  }
  http.end();
}

long getDistanceCM() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duration = pulseIn(ECHO, HIGH);
  return duration * 0.034 / 2;
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(LED_PIN, OUTPUT); pinMode(BUZZER, OUTPUT);
  
  WiFi.begin(ssid, password);
  Serial.print("WiFi Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nConnected.");

  mqttClient.setServer(mqtt_server, 1883);
  fbClient.setInsecure();
}

void loop() {
  if (!mqttClient.connected()) {
    if (mqttClient.connect(("AMB_" + String(random(999))).c_str())) {
      Serial.println("MQTT Connected");
    }
  }
  mqttClient.loop();

  long d = getDistanceCM();
  unsigned long now = millis();

  // --- CRITICAL ALERT LOGIC (Distance < 10cm) ---
  if (d > 0 && d <= 10 && (now - lastPublish > 1000)) {
    String msg = "AMBULANCE NEAR! MOVE ASIDE";
    
    // 1. MQTT Alert to Car
    mqttClient.publish("car/alert", msg.c_str());
    Serial.println("SENT: car/alert");
    
    // 2. Local Feedback
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER, HIGH);
    delay(200);
    digitalWrite(BUZZER, LOW);
    
    // 3. Firebase Log (POST for History)
    String json = "{\"message\":\"" + msg + "\",\"distance\":" + String(d) + ",\"time\":" + String(millis()) + "}";
    sendToFirebase("alerts.json", json, "POST"); // <--- CHANGED TO POST
    
    lastPublish = now;
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  // --- TRAFFIC SIGNAL LOGIC (Distance < 20cm) ---
  // Send status to traffic light
  if (d > 0 && d <= 20 && (now - lastPublish > 1500)) {
    mqttClient.publish("traffic/ambulance", "approaching");
    lastPublish = now;
  }

  // --- LIVE TRACKING LOGIC ---
  // Update map location/status every 3 seconds (PUT for Overwrite)
  if (now - lastFirebase > 3000) {
    String status = (d > 0 && d <= 20) ? "Approaching Traffic" : "En Route";
    // Mock location for demo (In real life, use GPS)
    String json = "{\"id\":\"" + deviceId + "\",\"status\":\"" + status + "\",\"location\":{\"lat\":12.9716,\"lng\":77.5946}}";
    sendToFirebase("ambulance/" + deviceId + ".json", json, "PUT"); // <--- KEEP AS PUT
    lastFirebase = now;
  }
}
/* SMART TRAFFIC CONTROLLER - FINAL PRODUCTION VERSION
   ---------------------------------------------------
   - Normal Cycle: S1 -> S2 -> S3 -> S4 (15s Green, 3s Yellow)
   - Ambulance Override: Force S2 Green for 20s
   - Resume Logic: Returns to the interrupted signal after ambulance clears
   - Hardware: ESP32, 4 Traffic Signals, 2 Hall Sensors, MQTT, Firebase
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ==========================================
// 1. CONFIGURATION & PINS
// ==========================================

const char* ssid = "Dhanush";
const char* password = "samsungm";

const char* mqtt_server = "broker.hivemq.com";
const char* firebase_base = "https://iot-ambulance-traffic-default-rtdb.asia-southeast1.firebasedatabase.app";

// --- TRAFFIC LIGHT PINS ---
// Signal 1
#define S1_RED    5
#define S1_YELLOW 18
#define S1_GREEN  19
// Signal 2 (Ambulance Lane)
#define S2_RED    21
#define S2_YELLOW 16
#define S2_GREEN  17
// Signal 3
#define S3_RED    25
#define S3_YELLOW 26
#define S3_GREEN  27
// Signal 4
#define S4_RED    32
#define S4_YELLOW 33
#define S4_GREEN  14

// --- SENSOR PINS ---
#define HALL1 22  
#define HALL2 23  // WARNING: If this pin is stuck LOW, change to Pin 23 or 15!

// --- TIMING CONFIG (Milliseconds) ---
const unsigned long T_GREEN = 15000;    // 15 Seconds Normal Green
const unsigned long T_YELLOW = 3000;    // 3 Seconds Yellow
const unsigned long T_AMB_HOLD = 20000; // 20 Seconds Ambulance Green

// ==========================================
// 2. GLOBAL VARIABLES
// ==========================================

WiFiClient mqttNet;
PubSubClient mqttClient(mqttNet);
WiFiClientSecure fbClient;

// System States
enum SystemMode { MODE_NORMAL, MODE_AMBULANCE, MODE_RECOVERY };
SystemMode currentMode = MODE_NORMAL;

enum TrafficLightState { LIGHT_GREEN, LIGHT_YELLOW };
TrafficLightState lightState = LIGHT_GREEN;

int activeSignal = 1;       // Currently active signal (1-4)
int interruptedSignal = 1;  // To remember where we were before ambulance
unsigned long cycleStartTime = 0; 
unsigned long ambulanceTimer = 0;

// Optimization to prevent spamming WiFi
int lastPublishedSig = -1;
String lastPublishedColor = "";

// ==========================================
// 3. HARDWARE HELPERS
// ==========================================

// Turn OFF all lights to prevent collisions
void setAllRed() {
  digitalWrite(S1_RED, HIGH); digitalWrite(S1_YELLOW, LOW); digitalWrite(S1_GREEN, LOW);
  digitalWrite(S2_RED, HIGH); digitalWrite(S2_YELLOW, LOW); digitalWrite(S2_GREEN, LOW);
  digitalWrite(S3_RED, HIGH); digitalWrite(S3_YELLOW, LOW); digitalWrite(S3_GREEN, LOW);
  digitalWrite(S4_RED, HIGH); digitalWrite(S4_YELLOW, LOW); digitalWrite(S4_GREEN, LOW);
}

// Send data to Firebase
void sendFirebase(int sig, String state) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  fbClient.setInsecure();
  String url = String(firebase_base) + "/traffic/signal" + String(sig) + ".json";
  http.begin(fbClient, url);
  http.addHeader("Content-Type", "application/json");
  String json = "{\"state\":\"" + state + "\",\"timestamp\":" + String(millis()) + "}";
  http.PUT(json);
  http.end();
}

// Control a specific signal
void setSignal(int sig, String color) {
  // Check duplicates to save WiFi bandwidth
  if (sig == lastPublishedSig && color == lastPublishedColor) return;
  lastPublishedSig = sig;
  lastPublishedColor = color;

  setAllRed(); // Safety Reset

  int r, y, g;
  switch (sig) {
    case 1: r=S1_RED; y=S1_YELLOW; g=S1_GREEN; break;
    case 2: r=S2_RED; y=S2_YELLOW; g=S2_GREEN; break;
    case 3: r=S3_RED; y=S3_YELLOW; g=S3_GREEN; break;
    case 4: r=S4_RED; y=S4_YELLOW; g=S4_GREEN; break;
    default: return;
  }

  // Apply State
  if (color == "GREEN") {
    digitalWrite(g, HIGH); digitalWrite(y, LOW); digitalWrite(r, LOW);
  } else if (color == "YELLOW") {
    digitalWrite(g, LOW); digitalWrite(y, HIGH); digitalWrite(r, LOW);
  } else { // RED
    digitalWrite(g, LOW); digitalWrite(y, LOW); digitalWrite(r, HIGH);
  }

  // Publish
  if (mqttClient.connected()) {
    mqttClient.publish("traffic/status", ("Signal " + String(sig) + " " + color).c_str());
  }
  sendFirebase(sig, color);
}

void advanceSignal() {
  activeSignal++;
  if (activeSignal > 4) activeSignal = 1;
}

// ==========================================
// 4. SETUP
// ==========================================

void setup() {
  Serial.begin(115200);

  // Pin Modes
  pinMode(HALL1, INPUT_PULLUP);
  pinMode(HALL2, INPUT_PULLUP);
  
  pinMode(S1_RED, OUTPUT); pinMode(S1_YELLOW, OUTPUT); pinMode(S1_GREEN, OUTPUT);
  pinMode(S2_RED, OUTPUT); pinMode(S2_YELLOW, OUTPUT); pinMode(S2_GREEN, OUTPUT);
  pinMode(S3_RED, OUTPUT); pinMode(S3_YELLOW, OUTPUT); pinMode(S3_GREEN, OUTPUT);
  pinMode(S4_RED, OUTPUT); pinMode(S4_YELLOW, OUTPUT); pinMode(S4_GREEN, OUTPUT);

  // WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println(" Connected!");

  // Server Setup
  mqttClient.setServer(mqtt_server, 1883);
  fbClient.setInsecure();

  // Initial State: Signal 1 Green
  setSignal(1, "GREEN");
  activeSignal = 1;
  cycleStartTime = millis();
}

// ==========================================
// 5. MAIN LOOP
// ==========================================

void loop() {
  // MQTT Keep-Alive
  if (!mqttClient.connected()) {
    if (mqttClient.connect(("TRAFFIC_" + String(random(999))).c_str())) {
      mqttClient.subscribe("traffic/command");
    }
  }
  mqttClient.loop();

  unsigned long now = millis();

  // Read Sensors (Active LOW)
  bool isAmbulance = (digitalRead(HALL1) == LOW || digitalRead(HALL2) == LOW);

  // Debug Print (Every 1 sec)
  if (now % 1000 < 20) {
    Serial.print("MODE: "); Serial.print(currentMode);
    Serial.print(" | Sig: "); Serial.print(activeSignal);
    Serial.print(" | Sensors: "); 
    Serial.print(digitalRead(HALL1)); Serial.print("/"); Serial.println(digitalRead(HALL2));
  }

  // ------------------------------------
  // STATE 1: DETECT AMBULANCE
  // ------------------------------------
  if (isAmbulance && currentMode == MODE_NORMAL) {
    Serial.println("🚨 AMBULANCE DETECTED! Saving state & Switching to Signal 2.");
    
    interruptedSignal = activeSignal; // Remember where we were
    currentMode = MODE_AMBULANCE;
    ambulanceTimer = now;
    
    setSignal(2, "GREEN"); // Force Ambulance Lane Green
    return;
  }

  // ------------------------------------
  // STATE 2: AMBULANCE OVERRIDE
  // ------------------------------------
  if (currentMode == MODE_AMBULANCE) {
    // If ambulance is still there, keep resetting the 20s timer
    if (isAmbulance) {
      ambulanceTimer = now;
    }

    // Exit Condition: Ambulance Gone AND 20s passed
    if (!isAmbulance && (now - ambulanceTimer > T_AMB_HOLD)) {
      Serial.println("✅ AMBULANCE CLEARED. Transitioning to Recovery.");
      currentMode = MODE_RECOVERY;
      cycleStartTime = now;
      setSignal(2, "YELLOW"); // Safety Transition
    }
    return;
  }

  // ------------------------------------
  // STATE 3: RECOVERY (Yellow Light)
  // ------------------------------------
  if (currentMode == MODE_RECOVERY) {
    // Wait 3 seconds for Signal 2 Yellow to finish
    if (now - cycleStartTime > T_YELLOW) {
      Serial.print("🔄 RESUMING CYCLE AT SIGNAL: ");
      Serial.println(interruptedSignal);
      
      currentMode = MODE_NORMAL;
      
      // Resume the interrupted signal
      activeSignal = interruptedSignal;
      lightState = LIGHT_GREEN;
      
      setSignal(activeSignal, "GREEN");
      cycleStartTime = now; // Give it a full green cycle
    }
    return;
  }

  // ------------------------------------
  // STATE 4: NORMAL TRAFFIC CYCLE
  // ------------------------------------
  if (currentMode == MODE_NORMAL) {
    
    // Green Phase logic
    if (lightState == LIGHT_GREEN) {
      if (now - cycleStartTime > T_GREEN) {
        lightState = LIGHT_YELLOW;
        setSignal(activeSignal, "YELLOW");
        cycleStartTime = now;
      }
    }
    // Yellow Phase logic
    else if (lightState == LIGHT_YELLOW) {
      if (now - cycleStartTime > T_YELLOW) {
        advanceSignal(); // Move 1->2->3->4->1
        lightState = LIGHT_GREEN;
        setSignal(activeSignal, "GREEN");
        cycleStartTime = now;
      }
    }
  }

  delay(20); // Stability Delay
}
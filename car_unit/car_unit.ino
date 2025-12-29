#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- CONFIGURATION ---
const char* ssid = "Dhanush";
const char* password = "samsungm";
const char* mqtt_server = "broker.hivemq.com";

// --- PINS ---
#define BUZZER 19
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- STATE VARIABLES ---
String lastAlertMsg = "";
String lastTrafficMsg = "";

unsigned long lastAlertTime = 0;
unsigned long lastTrafficTime = 0;

// Timeouts (How long to keep showing a message after receiving it)
const long ALERT_TIMEOUT = 2500;   // Keep alert active for 2.5s after last MQTT msg
const long TRAFFIC_TIMEOUT = 5000; // Keep traffic active for 5s after last MQTT msg

void showOLED(String title, String msg, bool invert) {
  display.clearDisplay();
  
  if (invert) {
    display.fillScreen(SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }

  display.setTextSize(2);
  display.setCursor(0, 5);
  display.println(title);
  
  display.setTextSize(1);
  display.setCursor(0, 35);
  display.println(msg);
  
  display.display();
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String top = String(topic);

  Serial.println("Msg: " + top + " -> " + msg);

  if (top == "car/alert") {
    lastAlertMsg = msg;
    lastAlertTime = millis();
    
    // Quick Buzzer Beep on every new packet to indicate urgency
    digitalWrite(BUZZER, HIGH);
    delay(100); 
    digitalWrite(BUZZER, LOW);
  } 
  else if (top == "traffic/status") {
    lastTrafficMsg = msg;
    lastTrafficTime = millis();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 failed"));
    for(;;);
  }
  showOLED("CAR UNIT", "Connecting...", false);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Connected");

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(callback);
}

void loop() {
  if (!mqttClient.connected()) {
    showOLED("Network", "Reconnecting...", false);
    String clientId = "CarUnit_" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("MQTT Connected");
      showOLED("Ready", "Safe Drive", false);
      mqttClient.subscribe("car/alert");
      mqttClient.subscribe("traffic/status");
    } else {
      delay(2000);
    }
  }
  mqttClient.loop();
  
  unsigned long now = millis();

  // Check which statuses are currently active
  bool isAlertActive = (now - lastAlertTime < ALERT_TIMEOUT);
  bool isTrafficActive = (now - lastTrafficTime < TRAFFIC_TIMEOUT);

  // --- DISPLAY LOGIC ---

  if (isAlertActive && isTrafficActive) {
    // Both active: Alternate every 2 seconds
    if ((now / 2000) % 2 == 0) {
       showOLED("ALERT!", lastAlertMsg, true); // Show Alert (Inverted)
    } else {
       showOLED("Traffic", lastTrafficMsg, false); // Show Traffic (Normal)
    }
  } 
  else if (isAlertActive) {
    // Only Alert is active
    showOLED("ALERT!", lastAlertMsg, true);
  } 
  else if (isTrafficActive) {
    // Only Traffic is active
    showOLED("Traffic", lastTrafficMsg, false);
  } 
  else {
    // Neither is active -> Normal Safe Drive state
    // We only update this occasionally to prevent screen flicker
    if (now % 1000 == 0) {
      showOLED("Ready", "Safe Drive", false);
    }
  }
}
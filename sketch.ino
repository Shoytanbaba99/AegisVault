#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>

// --- PIN ASSIGNMENTS ---
const int PIN_LDR           = 34; // paired with RBD-0995 & RBD-0868 (100K pull-down)
const int PIN_PIR           = 27; // RBD-0023 Motion Sensor
const int PIN_TAMPER_SWITCH = 14; // RBD-0761 Tactile Push Button (Uses Internal Pull-Up)
const int PIN_SERVO_DOOR    = 18; // RBD-0020 Door Deadbolt Servo
const int PIN_SERVO_DROP    = 13; // RBD-0020 Platform Concealment Servo
const int PIN_BUZZER_TIER1  = 25; // RBD-0753 5V Active Buzzer 1 (Pulsed Warning)
const int PIN_BUZZER_TIER2  = 26; // RBD-0753 5V Active Buzzer 2 (Continuous Siren)
const int PIN_LED_ALARM     = 32; // RBD-0768 Red LED (with 330R current limiter)
const int PIN_LED_STROBE    = 33; // RBD-1299 White LED (with 330R current limiter)

// --- SERVO LIMIT CONSTANTS ---
const int SERVO_DOOR_UNLOCKED       = 0;
const int SERVO_DOOR_LOCKED         = 90;
const int SERVO_PEDESTAL_SURFACE    = 0;
const int SERVO_PEDESTAL_CONCEALED  = 90;

// --- FSM STATES ---
enum SystemState {
  STATE_CALIBRATING,
  STATE_IDLE_ARMED,
  STATE_BREACH_TIER1,
  STATE_BREACH_TIER2,
  STATE_DISARMED
};

SystemState currentState = STATE_CALIBRATING;

Servo servoDoor;
Servo servoPedestal;

// --- REAL-WORLD NETWORK CONFIG ---
// UPDATE THESE WITH YOUR PHONE HOTSPOT OR LAB WI-FI CREDENTIALS
const char* WIFI_SSID     = "Your_Phone_Hotspot_Name"; 
const char* WIFI_PASSWORD = "Your_Hotspot_Password";
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

int baselineLux = 0;
int tripThreshold = 2500;

unsigned long lastHeartbeat   = 0;
unsigned long lastMqttRetry   = 0;
unsigned long lastStrobe      = 0;
unsigned long lastMotionTime  = 0;
unsigned long disarmTimestamp = 0; // Cooldown timer to prevent instant re-trigger
bool strobeState              = false;

void publishTelemetry(const char* eventState) {
  if (!mqttClient.connected()) return;

  String payload = "{";
  payload += "\"system\":\"AegisVault\",";
  payload += "\"state\":\"" + String(eventState) + "\",";
  payload += "\"ldr\":" + String(analogRead(PIN_LDR)) + ",";
  payload += "\"baseline\":" + String(baselineLux) + ",";
  payload += "\"threshold\":" + String(tripThreshold) + ",";
  payload += "\"pir\":" + String(digitalRead(PIN_PIR)) + ",";
  payload += "\"tamper\":" + String(digitalRead(PIN_TAMPER_SWITCH)) + ",";
  payload += "\"uptime_ms\":" + String(millis());
  payload += "}";

  mqttClient.publish("aegisvault/telemetry", payload.c_str());
  Serial.print("[MQTT TX] ");
  Serial.println(payload);
}

void setPhysicalStandby() {
  servoDoor.write(SERVO_DOOR_UNLOCKED);
  servoPedestal.write(SERVO_PEDESTAL_SURFACE);
  
  // Cut pure DC power cleanly to both physical active buzzers
  digitalWrite(PIN_BUZZER_TIER1, LOW);
  digitalWrite(PIN_BUZZER_TIER2, LOW);
  
  digitalWrite(PIN_LED_ALARM, LOW);
  digitalWrite(PIN_LED_STROBE, LOW);
}

void enterTier1Lockdown() {
  currentState = STATE_BREACH_TIER1;
  Serial.println("\n=======================================================");
  Serial.println("[ALERT] TIER 1 PERIMETER INTRUSION CONFIRMED!");
  Serial.println("  -> Engaging Door Deadbolt (Servo 18 -> 90 deg)");
  Serial.println("  -> Dropping Asset Pedestal (Servo 13 -> 90 deg)");
  Serial.println("  -> Initiating Pulsed Warning Alarm & Visual Strobes");
  Serial.println("=======================================================");

  servoDoor.write(SERVO_DOOR_LOCKED);
  servoPedestal.write(SERVO_PEDESTAL_CONCEALED);
  digitalWrite(PIN_LED_ALARM, HIGH);
  publishTelemetry("TIER_1_LOCKDOWN");
}

void enterTier2Escalation() {
  currentState = STATE_BREACH_TIER2;
  Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  Serial.println("[CRITICAL] TIER 2 ESCALATION: VAULT TAMPERING!");
  Serial.println("  -> Limit switch tripped in concealed compartment!");
  Serial.println("  -> Engaging Continuous 5V Tier 2 Active Siren");
  Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

  digitalWrite(PIN_BUZZER_TIER1, LOW);  // Ensure Tier 1 audio channel drops
  digitalWrite(PIN_BUZZER_TIER2, HIGH); // Send constant 5V to drive physical active buzzer oscillator
  publishTelemetry("TIER_2_CRITICAL_BREACH");
}

void enterDisarmedState() {
  currentState = STATE_DISARMED;
  disarmTimestamp = millis();
  lastMotionTime = 0; // Wipe pending PIR triggers

  Serial.println("\n-------------------------------------------------------");
  Serial.println("[RESET] Disarm signal received! Actuators resetting.");
  Serial.println("  -> Unlocking deadbolt (Servo 18 -> 0 deg)");
  Serial.println("  -> Raising pedestal (Servo 13 -> 0 deg)");
  Serial.println("  -> Silencing all sirens. Disarm cooldown: 5 seconds.");
  Serial.println("-------------------------------------------------------");

  setPhysicalStandby();
  publishTelemetry("SYSTEM_DISARMED");
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.print("[MQTT RX] Topic: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  Serial.println(msg);

  if (msg == "DISARM" || msg == "RESET") {
    enterDisarmedState();
  }
}

void connectNetwork() {
  Serial.print("[Wi-Fi] Connecting to: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Connected! Local IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[Wi-Fi] Timed out. Operating in offline failsafe mode.");
  }
}

void maintainBrokerConnection() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) {
      lastMqttRetry = now;
      Serial.print("[MQTT] Connecting to broker... ");
      String clientId = "AegisNode-" + String(random(0xffff), HEX);

      if (mqttClient.connect(clientId.c_str())) {
        Serial.println("Connected!");
        mqttClient.subscribe("aegisvault/commands");
        publishTelemetry("NODE_ONLINE");
      } else {
        Serial.print("Failed (rc=");
        Serial.print(mqttClient.state());
        Serial.println("). Retrying in 5s...");
      }
    }
  } else {
    mqttClient.loop();
  }
}

void calibrateOptics() {
  Serial.println("\n[CALIBRATION] Measuring resting optical baseline...");
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(PIN_LDR);
    delay(50);
  }
  baselineLux = sum / 20;

  if (baselineLux > 2000) {
    Serial.println("  -> WARNING: Sensor was dark during boot! Clamping baseline to 1000.");
    baselineLux = 1000;
  }
  
  tripThreshold = 2500;

  Serial.print("  -> Baseline Level (Light): ");
  Serial.println(baselineLux);
  Serial.print("  -> Break Threshold (Darkness >): ");
  Serial.println(tripThreshold);

  currentState = STATE_IDLE_ARMED;
  Serial.println("[FSM] Status: System Fully Armed and Monitoring.\n");
}

void handleSignalingPatterns() {
  unsigned long now = millis();

  if (currentState == STATE_BREACH_TIER1) {
    if (now - lastStrobe >= 250) {
      lastStrobe = now;
      strobeState = !strobeState;
      digitalWrite(PIN_LED_STROBE, strobeState ? HIGH : LOW);
      
      // Pulse physical active buzzer state symmetrically along with strobe pacing
      digitalWrite(PIN_BUZZER_TIER1, strobeState ? HIGH : LOW);
    }
  } else if (currentState == STATE_BREACH_TIER2) {
    if (now - lastStrobe >= 80) {
      lastStrobe = now;
      strobeState = !strobeState;
      digitalWrite(PIN_LED_STROBE, strobeState ? HIGH : LOW);
      digitalWrite(PIN_LED_ALARM, !strobeState ? HIGH : LOW);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_TAMPER_SWITCH, INPUT_PULLUP);

  pinMode(PIN_BUZZER_TIER1, OUTPUT);
  pinMode(PIN_BUZZER_TIER2, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_LED_STROBE, OUTPUT);

  servoDoor.setPeriodHertz(50);
  servoPedestal.setPeriodHertz(50);
  servoDoor.attach(PIN_SERVO_DOOR, 500, 2400);
  servoPedestal.attach(PIN_SERVO_DROP, 500, 2400);

  setPhysicalStandby();
  connectNetwork();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  calibrateOptics();
}

void loop() {
  maintainBrokerConnection();
  handleSignalingPatterns();

  int ldrVal    = analogRead(PIN_LDR);
  int pirVal    = digitalRead(PIN_PIR);
  int tamperVal = digitalRead(PIN_TAMPER_SWITCH);

  // Latch motion for 3 seconds
  if (pirVal == HIGH) {
    lastMotionTime = millis();
  }
  bool motionActive = (millis() - lastMotionTime < 3000);
  bool beamCut = (ldrVal > tripThreshold);

  // Periodic Telemetry Heartbeat (every 5 seconds)
  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    if (currentState == STATE_IDLE_ARMED) {
      publishTelemetry("HEARTBEAT_NORMAL");
      Serial.print("  [DEBUG LIVE] LDR: ");
      Serial.print(ldrVal);
      Serial.print(" (Cut? ");
      Serial.print(beamCut ? "YES" : "NO");
      Serial.print(") | PIR Motion Latched? ");
      Serial.println(motionActive ? "YES" : "NO");
    }
  }

  // --- KEYSTROKE INTERCEPT WITH ECHO & BUFFER FLUSH ---
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    Serial.print("\n[KEYBOARD DETECTED] Key pressed: '");
    Serial.print(cmd);
    Serial.println("'");

    if (cmd == 'r' || cmd == 'R') {
      enterDisarmedState();
    }

    // Flush any leftover newline or carriage return characters
    while (Serial.available() > 0) {
      Serial.read();
    }
  }

  // --- FINITE STATE MACHINE ---
  switch (currentState) {
    case STATE_IDLE_ARMED: {
      if (beamCut && motionActive) {
        enterTier1Lockdown();
      }
      break;
    }

    case STATE_BREACH_TIER1: {
      if (tamperVal == LOW) {
        enterTier2Escalation();
      }
      break;
    }

    case STATE_BREACH_TIER2: {
      break;
    }

    case STATE_DISARMED: {
      // 5-second cooldown gives you time to align your laser pointer clean on the LDR
      if (millis() - disarmTimestamp > 5000) {
        if (!beamCut) {
          currentState = STATE_IDLE_ARMED;
          Serial.println("\n[ARMED] Cooldown complete and beam clear. Vault re-armed.");
          publishTelemetry("SYSTEM_RESTORED_ARMED");
        } else {
          static unsigned long lastWarn = 0;
          if (millis() - lastWarn > 3000) {
            lastWarn = millis();
            Serial.println("[NOTICE] Disarmed. Please re-align laser onto the LDR to arm system.");
          }
        }
      }
      break;
    }

    case STATE_CALIBRATING:
      break;
  }

  delay(20);
}

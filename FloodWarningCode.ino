// ============================================================
//  IoT Flood Early Warning System
//  Hardware : ESP32 + HC-SR04 + FC-37 (digital only) + DHT11 + Buzzer + LED
//  Platform : ThingSpeak (free cloud storage)
//  Mode      : Continuous loop — reads and uploads every 16 sec
//  4-Stage alert: 0 beep / 1 beep / 2 beeps / 3 beeps + LED on
//
//  CALIBRATION NOTE:
//  Standalone HC-SR04 test with pot EMPTY measured ~15.7-16.7 cm
//  (not 15 cm as originally assumed). SENSOR_HEIGHT_CM below is
//  set to 16.0 cm based on that real measurement.
//  --> VERIFY: empty the pot fully, flash this code, and confirm
//      Water Level reads ~0.00 cm. If not, adjust SENSOR_HEIGHT_CM
//      up/down to match whatever your sensor reports with a
//      confirmed-empty pot. That live reading is always more
//      trustworthy than a ruler measurement for this geometry.
// ============================================================

#include <WiFi.h>
#include "ThingSpeak.h"
#include "DHT.h"

// ============================================================
//  STRUCT
// ============================================================

struct DHTReading {
  float temperature;
  float humidity;
  bool  success;
};

// ============================================================
//  SECTION 1 — YOUR DETAILS
// ============================================================

const char*   WIFI_SSID = "Ali04";
const char*   WIFI_PASS = "ali12345";
unsigned long CH_ID     = 3413395;
const char*   WRITE_KEY = "CC6QQXYSELWJ96HH";

// ============================================================
//  SECTION 2 — PIN DEFINITIONS
// ============================================================

#define TRIG_PIN      5
#define ECHO_PIN      18
#define RAIN_DO_PIN   14     // digital rain detect only
#define DHT_PIN       4
#define BUZZER_PIN    13
#define LED_PIN       2

// ============================================================
//  SECTION 3 — TIMING
//  16 sec gives margin above ThingSpeak's free-tier 15-sec
//  rate limit (avoids -401 "point not inserted" errors).
// ============================================================

#define READ_INTERVAL    16000
#define UPLOAD_INTERVAL  16000

// ============================================================
//  SECTION 4 — SENSOR / POT CONFIG
//
//  SENSOR_HEIGHT_CM = real measured empty-pot distance (16.0 cm),
//  NOT the tape-measure guess. This is the calibration fix.
//
//  4 stages scaled to pot height (7 cm), each band = 1.75 cm:
//    Stage 1 (Normal)  :  0.00 – 1.74 cm  → 0 beeps, LED off
//    Stage 2 (Watch)   :  1.75 – 3.49 cm  → 1 beep,  LED off
//    Stage 3 (Warning) :  3.50 – 5.24 cm  → 2 beeps, LED blink
//    Stage 4 (Danger)  : ≥ 5.25 cm        → 3 beeps, LED solid
// ============================================================

#define DHT_TYPE         DHT11
#define SENSOR_HEIGHT_CM 16.0    // <-- calibrated from real empty-pot reading
#define POT_HEIGHT_CM    7.0     // pot is 7 cm tall when full ("the sea")
#define MAX_DISTANCE_CM  400

#define STAGE2_CM   (POT_HEIGHT_CM * 0.25)   // 1.75 cm
#define STAGE3_CM   (POT_HEIGHT_CM * 0.50)   // 3.50 cm
#define STAGE4_CM   (POT_HEIGHT_CM * 0.75)   // 5.25 cm

// ============================================================
//  SECTION 5 — GLOBAL OBJECTS AND VARIABLES
// ============================================================

DHT        dht(DHT_PIN, DHT_TYPE);
WiFiClient client;

unsigned long lastReadTime      = 0;
unsigned long lastUploadTime    = 0;
unsigned long lastAlertBeepTime = 0;

int lastAlertLevel = 0;
int uploadCount    = 0;

float g_waterLevel  = 0;
int   g_rainDigital = 1;
float g_temperature = 0;
float g_humidity    = 0;
bool  g_sensorFault = false;

// ============================================================
//  SECTION 6 — FUNCTION: Connect to Wi-Fi
// ============================================================

bool connectWiFi() {
  Serial.println("");
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts % 5 == 0) {
      Serial.print(" [RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.print(" dBm] ");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Wi-Fi connected. IP: ");
    Serial.print(WiFi.localIP());
    Serial.print("  Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return true;
  } else {
    Serial.println("");
    Serial.print("Wi-Fi FAILED. Status code: ");
    Serial.println(WiFi.status());
    return false;
  }
}

// ============================================================
//  SECTION 7 — FUNCTION: Maintain Wi-Fi
// ============================================================

void maintainWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped. Reconnecting...");
    WiFi.disconnect();
    delay(1000);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected successfully.");
    } else {
      Serial.println("\nReconnection failed. Will try next cycle.");
    }
  }
}

// ============================================================
//  SECTION 8 — FUNCTION: Read water level (HC-SR04 median)
// ============================================================

float getWaterLevel() {
  float readings[5];
  int   timeoutCount = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long  duration   = pulseIn(ECHO_PIN, HIGH, 30000);
    float distanceCm = duration * 0.0343 / 2.0;

    if (duration == 0) {
      timeoutCount++;
    }

    if (distanceCm <= 0 || distanceCm > MAX_DISTANCE_CM) {
      distanceCm = MAX_DISTANCE_CM;
    }

    readings[i] = distanceCm;

    Serial.print("  HC-SR04 sample "); Serial.print(i + 1);
    Serial.print(" -> duration: "); Serial.print(duration);
    Serial.print(" us, distance: "); Serial.print(distanceCm);
    Serial.println(" cm");

    delay(30);
  }

  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 5; j++) {
      if (readings[i] > readings[j]) {
        float temp  = readings[i];
        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }

  g_sensorFault = (timeoutCount >= 3);
  if (g_sensorFault) {
    Serial.println("  *** HC-SR04 FAULT: no echo on most samples. ***");
  }

  float waterLevel = SENSOR_HEIGHT_CM - readings[2];
  if (waterLevel < 0) waterLevel = 0;
  if (waterLevel > POT_HEIGHT_CM) waterLevel = POT_HEIGHT_CM;

  return waterLevel;
}

// ============================================================
//  SECTION 9 — FUNCTION: Read DHT11
// ============================================================

DHTReading readDHT() {
  DHTReading result;
  result.success     = false;
  result.temperature = g_temperature;
  result.humidity    = g_humidity;

  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (!isnan(temp) && !isnan(hum)) {
    result.temperature = temp;
    result.humidity    = hum;
    result.success     = true;
  } else {
    Serial.println("DHT11 read failed — using last valid reading.");
  }

  return result;
}

// ============================================================
//  SECTION 10 — FUNCTION: 4-Stage Alert System
// ============================================================

void triggerAlert(float waterLevel) {
  unsigned long now = millis();

  if (waterLevel >= STAGE4_CM) {
    Serial.println("*** STAGE 4 DANGER: Flood level critical! ***");
    digitalWrite(LED_PIN, HIGH);

    if (now - lastAlertBeepTime >= 15000 || lastAlertLevel < 3) {
      lastAlertBeepTime = now;
      for (int i = 0; i < 3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(150);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
      }
      Serial.println("Stage 4 alert: 3 rapid beeps.");
    } else {
      Serial.print("Next danger beep in ");
      Serial.print((15000 - (now - lastAlertBeepTime)) / 1000);
      Serial.println(" seconds.");
    }
    lastAlertLevel = 3;

  } else if (waterLevel >= STAGE3_CM) {
    Serial.println("*** STAGE 3 WARNING: Water level rising fast! ***");
    for (int i = 0; i < 2; i++) {
      digitalWrite(LED_PIN, HIGH); delay(150);
      digitalWrite(LED_PIN, LOW);  delay(150);
    }

    if (now - lastAlertBeepTime >= 15000 || lastAlertLevel < 2) {
      lastAlertBeepTime = now;
      for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(200);
        digitalWrite(BUZZER_PIN, LOW);  delay(150);
      }
      Serial.println("Stage 3 alert: 2 beeps.");
    } else {
      Serial.print("Next warning beep in ");
      Serial.print((15000 - (now - lastAlertBeepTime)) / 1000);
      Serial.println(" seconds.");
    }
    lastAlertLevel = 2;

  } else if (waterLevel >= STAGE2_CM) {
    Serial.println("*** STAGE 2 WATCH: Water level elevated. ***");
    digitalWrite(LED_PIN, LOW);

    if (now - lastAlertBeepTime >= 15000 || lastAlertLevel < 1) {
      lastAlertBeepTime = now;
      digitalWrite(BUZZER_PIN, HIGH); delay(200);
      digitalWrite(BUZZER_PIN, LOW);
      Serial.println("Stage 2 alert: 1 beep.");
    } else {
      Serial.print("Next watch beep in ");
      Serial.print((15000 - (now - lastAlertBeepTime)) / 1000);
      Serial.println(" seconds.");
    }
    lastAlertLevel = 1;

  } else {
    if (lastAlertLevel > 0) {
      Serial.println("Water level back to normal — all clear.");
      digitalWrite(BUZZER_PIN, HIGH); delay(600);
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      Serial.println("Stage 1 normal. No alert.");
    }

    digitalWrite(LED_PIN,    LOW);
    digitalWrite(BUZZER_PIN, LOW);

    lastAlertLevel    = 0;
    lastAlertBeepTime = 0;
  }
}

// ============================================================
//  SECTION 11 — FUNCTION: Read all sensors
// ============================================================

void readAllSensors() {
  g_waterLevel  = getWaterLevel();
  g_rainDigital = digitalRead(RAIN_DO_PIN);

  DHTReading dhtData = readDHT();
  g_temperature = dhtData.temperature;
  g_humidity    = dhtData.humidity;

  Serial.println("");
  Serial.println("--- Sensor Reading ---");
  Serial.print("Water Level   : "); Serial.print(g_waterLevel);   Serial.println(" cm");
  if (g_sensorFault) {
    Serial.println("                (UNRELIABLE — HC-SR04 echo timeout)");
  }
  Serial.print("Rain          : "); Serial.println(g_rainDigital == 0 ? "YES — RAINING" : "NO — DRY");
  Serial.print("Temperature   : "); Serial.print(g_temperature);  Serial.println(" C");
  Serial.print("Humidity      : "); Serial.print(g_humidity);     Serial.println(" %");

  if      (g_waterLevel >= STAGE4_CM) Serial.println("Alert Stage   : 4 — DANGER");
  else if (g_waterLevel >= STAGE3_CM) Serial.println("Alert Stage   : 3 — WARNING");
  else if (g_waterLevel >= STAGE2_CM) Serial.println("Alert Stage   : 2 — WATCH");
  else                                Serial.println("Alert Stage   : 1 — NORMAL");

  triggerAlert(g_waterLevel);
}

// ============================================================
//  SECTION 12 — FUNCTION: Upload to ThingSpeak
// ============================================================

void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No Wi-Fi — skipping upload this cycle.");
    return;
  }

  int alertStage = 1;
  if      (g_waterLevel >= STAGE4_CM) alertStage = 4;
  else if (g_waterLevel >= STAGE3_CM) alertStage = 3;
  else if (g_waterLevel >= STAGE2_CM) alertStage = 2;

  Serial.println("");
  Serial.println("--- Uploading to ThingSpeak ---");
  Serial.print("Field 1 - Water Level (cm) : "); Serial.println(g_waterLevel);
  Serial.print("Field 2 - Rain Detected    : "); Serial.println(g_rainDigital == 0 ? "YES (0)" : "NO (1)");
  Serial.print("Field 3 - Temperature (C)  : "); Serial.println(g_temperature);
  Serial.print("Field 4 - Humidity (%)     : "); Serial.println(g_humidity);
  Serial.print("Field 5 - Alert Stage      : "); Serial.println(alertStage);

  ThingSpeak.setField(1, g_waterLevel);
  ThingSpeak.setField(2, g_rainDigital);
  ThingSpeak.setField(3, g_temperature);
  ThingSpeak.setField(4, g_humidity);
  ThingSpeak.setField(5, alertStage);

  int result = ThingSpeak.writeFields(CH_ID, WRITE_KEY);
  uploadCount++;

  Serial.print("Upload #"); Serial.print(uploadCount); Serial.print(" : ");
  if (result == 200) {
    Serial.println("ThingSpeak SUCCESS");
  } else {
    Serial.print("FAILED — Error code: ");
    Serial.println(result);
    if      (result == 400)  Serial.println("Error 400  : Check WRITE_KEY and CH_ID.");
    else if (result == -301) Serial.println("Error -301 : Cannot reach ThingSpeak (network/DNS issue).");
    else if (result == -302) Serial.println("Error -302 : Unexpected failure during write.");
    else if (result == -303) Serial.println("Error -303 : Failed to parse ThingSpeak's response.");
    else if (result == -304) Serial.println("Error -304 : Timeout waiting for ThingSpeak to respond.");
    else if (result == -401) Serial.println("Error -401 : Point not inserted — likely wrote faster than the 15-sec rate limit.");
    else if (result == 401)  Serial.println("Error 401  : Unauthorized. Check WRITE_KEY.");
    else if (result == 404)  Serial.println("Error 404  : Channel not found. Check CH_ID.");
  }
}

// ============================================================
//  SECTION 13 — SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("");
  Serial.println("========================================");
  Serial.println("  IoT Flood Early Warning System");
  Serial.println("  Chittagong, Bangladesh");
  Serial.println("  4-Stage Water Level Alert");
  Serial.print  ("  Pot height (full)  : "); Serial.print(POT_HEIGHT_CM);    Serial.println(" cm");
  Serial.print  ("  Sensor height      : "); Serial.print(SENSOR_HEIGHT_CM); Serial.println(" cm above pot bottom (calibrated)");
  Serial.println("========================================");

  pinMode(TRIG_PIN,    OUTPUT);
  pinMode(ECHO_PIN,    INPUT);
  pinMode(RAIN_DO_PIN, INPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  pinMode(LED_PIN,     OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  dht.begin();
  Serial.println("DHT11 sensor started.");
  delay(2000);

  bool wifiOk = false;
  for (int retry = 1; retry <= 3; retry++) {
    Serial.print("Wi-Fi attempt "); Serial.print(retry); Serial.println(" of 3...");
    wifiOk = connectWiFi();
    if (wifiOk) break;
    if (retry < 3) { Serial.println("Retrying in 3 seconds..."); delay(3000); }
  }

  if (wifiOk) {
    ThingSpeak.begin(client);
    Serial.println("ThingSpeak ready.");
  } else {
    Serial.println("Starting without Wi-Fi. Readings continue locally.");
  }

  Serial.println("Startup test...");
  digitalWrite(BUZZER_PIN, HIGH); delay(100);
  digitalWrite(BUZZER_PIN, LOW);  delay(100);
  digitalWrite(BUZZER_PIN, HIGH); delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, HIGH); delay(300);
  digitalWrite(LED_PIN, LOW);

  Serial.println("");
  Serial.println("System ready. Alert stages:");
  Serial.print("  Stage 1 (Normal)  : 0.00 – "); Serial.print(STAGE2_CM - 0.01, 2); Serial.println(" cm  → silent, LED off");
  Serial.print("  Stage 2 (Watch)   : "); Serial.print(STAGE2_CM); Serial.print(" – "); Serial.print(STAGE3_CM - 0.01, 2); Serial.println(" cm  → 1 beep, LED off");
  Serial.print("  Stage 3 (Warning) : "); Serial.print(STAGE3_CM); Serial.print(" – "); Serial.print(STAGE4_CM - 0.01, 2); Serial.println(" cm  → 2 beeps, LED blink");
  Serial.print("  Stage 4 (Danger)  : "); Serial.print(STAGE4_CM); Serial.println(" cm+  → 3 beeps, LED solid ON");
  Serial.println("========================================");

  lastReadTime   = millis() - READ_INTERVAL;
  lastUploadTime = millis() - UPLOAD_INTERVAL;
}

// ============================================================
//  SECTION 14 — LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime   = now;
    lastUploadTime = now;

    maintainWiFi();
    readAllSensors();
    uploadToThingSpeak();
  }

  delay(100);
}

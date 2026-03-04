#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>  // Install via Library Manager: "ArduinoJson" by Benoit Blanchon

// ── WiFi Credentials ─────────────────────────────────────────────
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"

// ── Backend Config ───────────────────────────────────────────────
#define BASE_URL          "http://192.168.1.100:8000"   // Your FastAPI server IP
#define ENDPOINT_HEALTH   BASE_URL "/health"            // GET  - Health check
#define ENDPOINT_ATTEND   BASE_URL "/attendance/log"    // POST - Log attendance

// ── Pin Definitions ──────────────────────────────────────────────
#define FP_RX_PIN 16
#define FP_TX_PIN 17

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

uint8_t id;

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial); delay(100);

  Serial.println("\n\n=== ESP32 Fingerprint Attendance System ===");

  // Connect to WiFi
  Serial.print("[WIFI] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());

  // Initialize fingerprint sensor
  fingerSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  //finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("[OK] Fingerprint sensor found!");
  } else {
    Serial.println("[ERROR] Fingerprint sensor not found. Check wiring.");
    while (1) { delay(1); }
  }

  finger.getParameters();
  Serial.println(F("\n--- Sensor Parameters ---"));
  Serial.print(F("  Status       : 0x")); Serial.println(finger.status_reg, HEX);
  Serial.print(F("  System ID    : 0x")); Serial.println(finger.system_id, HEX);
  Serial.print(F("  Capacity     : "));   Serial.println(finger.capacity);
  Serial.print(F("  Security Lvl : "));   Serial.println(finger.security_level);
  Serial.print(F("  Baud Rate    : "));   Serial.println(finger.baud_rate);

  finger.getTemplateCount();
  Serial.print(F("  Stored IDs   : "));   Serial.println(finger.templateCount);
  Serial.println(F("-------------------------\n"));

  printMenu();
}

// ── Menu ─────────────────────────────────────────────────────────
void printMenu() {
  Serial.println("Commands:");
  Serial.println("  E - Enroll a new fingerprint");
  Serial.println("  S - Scan / verify fingerprint (attendance)");
  Serial.println("  D - Delete a fingerprint by ID");
  Serial.println("  C - Count stored fingerprints");
  Serial.println("  B - POST test (log dummy attendance)");
  Serial.println("  H - GET test  (server health check)");
}

// ── Read Number from Serial ───────────────────────────────────────
uint8_t readnumber(void) {
  uint8_t num = 0;
  while (num == 0) {
    while (!Serial.available());
    num = Serial.parseInt();
  }
  return num;
}

// ── Main Loop ────────────────────────────────────────────────────
void loop() {
  if (Serial.available()) {
    char cmd = toupper(Serial.read());

    switch (cmd) {
      case 'E':
        Serial.println("\n[ENROLL] Enter ID (1–127):");
        id = readnumber();
        if (id == 0 || id > 127) {
          Serial.println("[ERROR] Invalid ID. Must be 1–127.");
          break;
        }
        Serial.print("Enrolling ID #"); Serial.println(id);
        while (!getFingerprintEnroll());
        printMenu();
        break;

      case 'S':
        Serial.println("\n[SCAN] Place finger on sensor...");
        getFingerprintID();
        printMenu();
        break;

      case 'D':
        Serial.println("\n[DELETE] Enter ID to delete (1–127):");
        id = readnumber();
        deleteFingerprint(id);
        printMenu();
        break;

      case 'C':
        finger.getTemplateCount();
        Serial.print("\n[COUNT] Stored fingerprints: ");
        Serial.println(finger.templateCount);
        printMenu();
        break;

      case 'B':
        // POST - Send a dummy attendance record to backend
        postAttendance(1, 95);  // fingerID=1, confidence=95
        printMenu();
        break;

      case 'H':
        // GET - Check if backend server is alive
        getHealthCheck();
        printMenu();
        break;
    }
  }
}

// ── HTTP GET - Health Check ───────────────────────────────────────
void getHealthCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    return;
  }

  HTTPClient http;
  http.begin(ENDPOINT_HEALTH);

  Serial.println("\n[GET] " + String(ENDPOINT_HEALTH));
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[GET] Status code : "); Serial.println(httpCode);
    Serial.print("[GET] Response     : "); Serial.println(response);
  } else {
    Serial.print("[GET] Request failed, error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// ── HTTP POST - Log Attendance ────────────────────────────────────
void postAttendance(uint16_t fingerprintID, uint16_t confidence) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    return;
  }

  // Build JSON payload
  JsonDocument doc;
  doc["fingerprint_id"] = fingerprintID;
  doc["confidence"]     = confidence;
  doc["device_id"]      = "esp32-attendance-01";  // Useful if you have multiple devices

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(ENDPOINT_ATTEND);
  http.addHeader("Content-Type", "application/json");

  Serial.println("\n[POST] " + String(ENDPOINT_ATTEND));
  Serial.println("[POST] Payload: " + payload);

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[POST] Status code : "); Serial.println(httpCode);
    Serial.print("[POST] Response     : "); Serial.println(response);
  } else {
    Serial.print("[POST] Request failed, error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// ── Enroll Fingerprint ────────────────────────────────────────────
uint8_t getFingerprintEnroll() {
  int p = -1;

  Serial.print("\nWaiting for finger to enroll as ID #"); Serial.println(id);
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK)             Serial.println("Image captured.");
    else if (p == FINGERPRINT_NOFINGER)  Serial.print(".");
    else if (p == FINGERPRINT_IMAGEFAIL) Serial.println("[ERROR] Imaging error.");
    else                                  Serial.println("[ERROR] Communication error.");
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    return p;
  }

  Serial.println("Remove finger...");
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER);

  Serial.println("Place the SAME finger again...");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK)             Serial.println("Image captured.");
    else if (p == FINGERPRINT_NOFINGER)  Serial.print(".");
    else                                  Serial.println("[ERROR] Communication error.");
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    return p;
  }

  p = finger.createModel();
  if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("[ERROR] Fingerprints did not match. Try again.");
    return p;
  } else if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not create model.");
    return p;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.print("[SUCCESS] Fingerprint stored as ID #");
    Serial.println(id);
  } else {
    Serial.println("[ERROR] Failed to store fingerprint.");
    return p;
  }

  return true;
}

// ── Scan / Verify Fingerprint (Attendance) ────────────────────────
void getFingerprintID() {
  int p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) {
    Serial.println("[SCAN] No finger detected.");
    return;
  }
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Imaging error.");
    return;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    return;
  }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    Serial.println("\n============================");
    Serial.println("  ✔ FINGERPRINT MATCHED");
    Serial.print("  ID        : #"); Serial.println(finger.fingerID);
    Serial.print("  Confidence: ");  Serial.println(finger.confidence);
    Serial.println("============================\n");
    postAttendance(finger.fingerID, finger.confidence);  // ← Real attendance POST
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[SCAN] No matching fingerprint found.");
  } else {
    Serial.println("[ERROR] Search failed.");
  }
}

// ── Delete Fingerprint ────────────────────────────────────────────
void deleteFingerprint(uint8_t delID) {
  if (finger.deleteModel(delID) == FINGERPRINT_OK) {
    Serial.print("[DELETE] Fingerprint ID #");
    Serial.print(delID);
    Serial.println(" deleted.");
  } else {
    Serial.println("[ERROR] Could not delete fingerprint.");
  }
}

#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#ifndef WIFI_SSID
#define WIFI_SSID     "your_wifi_ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_wifi_password"
#endif

#ifndef DEVICE_ID
#define DEVICE_ID "esp32-attendance-01"
#endif

#ifndef DEVICE_API_KEY
#define DEVICE_API_KEY "REPLACE_DEVICE_API_KEY"
#endif

#ifndef BASE_URL
#define BASE_URL "http://192.168.1.11:8080/api"
#endif

// Development transport mode:
// - HTTPS support is intentionally commented out for now while the device flow is still changing.
// - Re-enable WiFiClientSecure, TLS_CA_CERT, and the secure branch in beginApiRequest() before production.
#ifndef TLS_CA_CERT
#define TLS_CA_CERT ""
#endif

#define ENDPOINT_HEALTH BASE_URL "/health"
#define FP_RX_PIN 16
#define FP_TX_PIN 17
#define ENROLLMENT_POLL_INTERVAL_MS 5000UL
#define ENROLLMENT_RESULT_RETRY_INTERVAL_MS 15000UL
#define HTTP_TIMEOUT_MS 5000
#define TEMPLATE_EXPORT_TIMEOUT_MS 3000
#define TEMPLATE_INITIAL_BUFFER_SIZE 512
#define TEMPLATE_MAX_BUFFER_SIZE 2048
#define TEMPLATE_PACKET_BUFFER_SIZE 256
#define TEMPLATE_STREAM_IDLE_MS 120
#define TEMPLATE_STREAM_DRAIN_TIMEOUT_MS 1500

#define LCD_ADDR 0x27
#define LCD_COLS 20
#define LCD_ROWS 4
#define LCD_RESULT_DISPLAY_MS 3000UL
#define AUTO_SCAN_INTERVAL_MS 2000UL

HardwareSerial fingerSerial(2);
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
WiFiClient insecureApiClient;
WiFiClientSecure secureApiClient;

struct EnrollmentJob {
  bool available = false;
  String id;
  String studentId;
  String studentName;
  uint16_t assignedSensorFingerprintId = 0;
};

enum EnrollmentResultDeliveryStatus {
  ENROLLMENT_RESULT_CONFIRMED,
  ENROLLMENT_RESULT_RETRY_LATER,
  ENROLLMENT_RESULT_REJECTED
};

struct PendingEnrollmentResult {
  bool active = false;
  EnrollmentJob job;
  bool success = false;
  String backupTemplateBase64;
  String failureReason;
  unsigned long nextAttemptAtMs = 0;
  uint32_t attemptCount = 0;
};

unsigned long lastEnrollmentPollMs = 0;
uint16_t activeSlotId = 0;
PendingEnrollmentResult pendingEnrollmentResult;

unsigned long lcdResultTimestamp = 0;
bool lcdResultActive = false;
unsigned long lastAutoScanAttemptMs = 0;
bool readyToScanShown = false;
bool autoScanAwaitingFingerRemoval = false;

void lcdInit() {
  delay(1000);
  Wire.begin(21, 22);
  Wire.setClock(10000);
  lcd.init();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcdPrintLine(0, "AMS");
  lcdPrintLine(1, "Initializing...");
}

void lcdPrintLine(uint8_t line, const char* text) {
  if (line >= LCD_ROWS) return;
  lcd.setCursor(0, line);
  uint8_t len = strlen(text);
  uint8_t pad = (len < LCD_COLS) ? (LCD_COLS - len) : 0;
  for (uint8_t i = 0; i < LCD_COLS; i++) {
    if (i < len && i < LCD_COLS) {
      lcd.print(text[i]);
    } else {
      lcd.print(' ');
    }
  }
}

void lcdPrintCenter(uint8_t line, const char* text) {
  if (line >= LCD_ROWS) return;
  uint8_t len = strlen(text);
  if (len >= LCD_COLS) {
    lcdPrintLine(line, text);
    return;
  }
  uint8_t pad = (LCD_COLS - len) / 2;
  lcd.setCursor(0, line);
  for (uint8_t i = 0; i < pad; i++) lcd.print(' ');
  lcd.print(text);
  for (uint8_t i = pad + len; i < LCD_COLS; i++) lcd.print(' ');
}

void updateLcdWifiLine(const char* text) {
  lcdPrintLine(1, text);
}

void updateLcdOpLine(const char* text) {
  lcdPrintLine(2, text);
}

void lcdFlashResult(const char* text) {
  lcdPrintLine(3, text);
  lcdResultTimestamp = millis();
  lcdResultActive = true;
}

void updateLcdResultTimeout() {
  if (lcdResultActive && millis() - lcdResultTimestamp >= LCD_RESULT_DISPLAY_MS) {
    lcdPrintLine(3, "");
    lcdResultActive = false;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(100);
  }
  delay(100);

  Serial.println("\n\n=== ESP32 Fingerprint Enrollment + Attendance Device ===");

  lcdInit();
  updateLcdOpLine("Connecting WiFi...");

  connectWiFi();

  fingerSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);

  if (!finger.verifyPassword()) {
    Serial.println("[ERROR] Fingerprint sensor not found. Check wiring.");
    updateLcdOpLine("Sensor error!");
    while (true) {
      delay(1);
    }
  }

  finger.getParameters();
  Serial.println(F("\n--- Sensor Parameters ---"));
  Serial.print(F("  Status       : 0x")); Serial.println(finger.status_reg, HEX);
  Serial.print(F("  System ID    : 0x")); Serial.println(finger.system_id, HEX);
  Serial.print(F("  Capacity     : ")); Serial.println(finger.capacity);
  Serial.print(F("  Security Lvl : ")); Serial.println(finger.security_level);
  Serial.print(F("  Baud Rate    : ")); Serial.println(finger.baud_rate);
  Serial.print(F("  Packet Len   : ")); Serial.println(finger.packet_len);
  finger.getTemplateCount();
  Serial.print(F("  Stored IDs   : ")); Serial.println(finger.templateCount);
  Serial.println(F("-------------------------\n"));

  updateLcdOpLine("Ready to scan");
  readyToScanShown = true;
  lcdFlashResult("Ready");

  printMenu();
}

void loop() {
  ensureWiFiConnected();
  processPendingEnrollmentResultIfDue();
  pollEnrollmentSessionIfNeeded(false);
  handleSerialCommands();
  updateLcdResultTimeout();
  scanFingerprintAutomaticallyIfDue();
}

void printMenu() {
  Serial.println("Commands:");
  Serial.println("  S - Scan / verify fingerprint (attendance)");
  Serial.println("  P - Poll backend for pending enrollment job now");
  Serial.println("  D - Delete a fingerprint by slot ID");
  Serial.println("  C - Count stored fingerprints");
  Serial.println("  H - GET test (server health check)");
  Serial.println("  R - Reset/clear pending enrollment result");
}

void handleSerialCommands() {
  if (!Serial.available()) {
    return;
  }

  char cmd = toupper(Serial.read());

  switch (cmd) {
    case 'S':
      Serial.println("\n[SCAN] Place finger on sensor...");
      updateLcdOpLine("Scanning...");
      readyToScanShown = false;
      if (getFingerprintID(true)) {
        autoScanAwaitingFingerRemoval = true;
      }
      showReadyToScan();
      printMenu();
      break;

    case 'P':
      Serial.println("\n[POLL] Checking backend for pending enrollment job...");
      updateLcdOpLine("Polling...");
      readyToScanShown = false;
      pollEnrollmentSessionIfNeeded(true);
      showReadyToScan();
      printMenu();
      break;

    case 'D':
      Serial.print("\n[DELETE] Enter slot ID to delete (1-");
      Serial.print(getMaxSensorSlotId());
      Serial.println("):");
      activeSlotId = readnumber();
      if (!isValidSensorSlot(activeSlotId, true)) {
        printMenu();
        break;
      }
      deleteFingerprint(activeSlotId);
      lcdFlashResult("Deleted slot #");
      Serial.println("[WARN] Backend registration is not removed automatically by local delete.");
      printMenu();
      break;

    case 'C':
      finger.getTemplateCount();
      Serial.print("\n[COUNT] Stored fingerprints: ");
      Serial.println(finger.templateCount);
      {
        String countMsg = "Count: ";
        countMsg += finger.templateCount;
        lcdFlashResult(countMsg.c_str());
      }
      printMenu();
      break;

    case 'H':
      updateLcdOpLine("Health check...");
      readyToScanShown = false;
      getHealthCheck();
      showReadyToScan();
      printMenu();
      break;

    case 'R':
      Serial.println("\n[RESET] Clearing pending enrollment result...");
      clearPendingEnrollmentResult();
      lcdFlashResult("Pending cleared");
      printMenu();
      break;
  }
}

void showReadyToScan() {
  if (readyToScanShown) {
    return;
  }

  updateLcdOpLine("Ready to scan");
  readyToScanShown = true;
}

void scanFingerprintAutomaticallyIfDue() {
  const unsigned long now = millis();
  if (now - lastAutoScanAttemptMs < AUTO_SCAN_INTERVAL_MS) {
    return;
  }

  lastAutoScanAttemptMs = now;

  if (autoScanAwaitingFingerRemoval) {
    int imageStatus = finger.getImage();
    if (imageStatus == FINGERPRINT_NOFINGER) {
      autoScanAwaitingFingerRemoval = false;
      showReadyToScan();
    } else {
      updateLcdOpLine("Remove finger");
      readyToScanShown = false;
    }
    return;
  }

  // First check if finger is present before showing "Scanning..."
  int imageStatus = finger.getImage();
  if (imageStatus == FINGERPRINT_NOFINGER) {
    // No finger present, keep showing ready state
    showReadyToScan();
    return;
  }

  // Finger detected, proceed with full scan
  updateLcdOpLine("Scanning...");
  readyToScanShown = false;
  if (getFingerprintID(true)) {
    autoScanAwaitingFingerRemoval = true;
  }
  showReadyToScan();
}

void connectWiFi() {
  Serial.print("[WIFI] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());

  String wifiLine = "WiFi: " + WiFi.localIP().toString();
  updateLcdWifiLine(wifiLine.c_str());
}

bool beginApiRequest(HTTPClient& http, const String& url) {
  if (url.startsWith("https://")) {
    if (strlen(TLS_CA_CERT) > 0) {
      secureApiClient.setCACert(TLS_CA_CERT);
    } else {
      secureApiClient.setInsecure();
    }
    return http.begin(secureApiClient, url);
  }

  if (url.startsWith("http://")) {
    // Use the legacy HTTPClient path first because this is the known-working behavior.
    if (http.begin(url)) {
      return true;
    }

    Serial.println("[HTTP] http.begin(url) failed, retrying with explicit WiFiClient.");
    return http.begin(insecureApiClient, url);
  }

  Serial.print("[HTTP] Unsupported URL scheme: ");
  Serial.println(url);
  return false;
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println("[WIFI] Connection lost. Reconnecting...");
  updateLcdWifiLine("WiFi: Reconnecting...");
  WiFi.disconnect();
  connectWiFi();
}

uint16_t readnumber(void) {
  uint16_t num = 0;
  while (num == 0) {
    while (!Serial.available()) {
      delay(10);
    }
    num = Serial.parseInt();
  }
  return num;
}

void pollEnrollmentSessionIfNeeded(bool force) {
  if (pendingEnrollmentResult.active) {
    return;
  }

  const unsigned long now = millis();
  if (!force && now - lastEnrollmentPollMs < ENROLLMENT_POLL_INTERVAL_MS) {
    return;
  }

  lastEnrollmentPollMs = now;

  EnrollmentJob job;
  if (!fetchPendingEnrollmentSession(job) || !job.available) {
    return;
  }

  processEnrollmentJob(job);
}

bool fetchPendingEnrollmentSession(EnrollmentJob& job) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    return false;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!beginApiRequest(http, String(BASE_URL) + "/fingerprint/devices/" + DEVICE_ID + "/enrollment-session")) {
    return false;
  }
  addDeviceHeaders(http);

  int httpCode = http.GET();
  if (httpCode == 204) {
    http.end();
    return true;
  }

  if (httpCode <= 0) {
    Serial.print("[POLL] Request failed: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  if (httpCode != 200) {
    Serial.print("[POLL] Unexpected status ");
    Serial.print(httpCode);
    Serial.print(": ");
    Serial.println(response);
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.print("[POLL] Failed to parse response: ");
    Serial.println(error.c_str());
    return false;
  }

  job.available = doc["success"] | false;
  job.id = String((const char*)(doc["enrollmentSessionId"] | ""));
  job.studentId = String((const char*)(doc["studentId"] | ""));
  job.studentName = String((const char*)(doc["studentName"] | ""));
  job.assignedSensorFingerprintId = static_cast<uint16_t>(doc["assignedSensorFingerprintId"] | 0);

  if (!job.available || job.id.length() == 0 || job.studentId.length() == 0) {
    Serial.println("[POLL] Backend returned an invalid enrollment payload.");
    return false;
  }

  if (!isValidSensorSlot(job.assignedSensorFingerprintId, true)) {
    Serial.println("[POLL] Rejecting enrollment job because the assigned slot is invalid for this sensor.");
    deliverEnrollmentResult(job, false, "", invalidSlotFailureMessage(job.assignedSensorFingerprintId));
    return false;
  }

  Serial.println("[POLL] Pending enrollment found.");
  Serial.print("  Student : "); Serial.println(job.studentName);
  Serial.print("  Student ID: "); Serial.println(job.studentId);
  Serial.print("  Slot    : "); Serial.println(job.assignedSensorFingerprintId);
  String enrollMsg = "Enroll: " + job.studentName;
  updateLcdOpLine(enrollMsg.substring(0, LCD_COLS).c_str());
  return true;
}

void processEnrollmentJob(const EnrollmentJob& job) {
  Serial.println("\n[ENROLL] Starting backend-driven enrollment...");
  Serial.print("[ENROLL] Student: "); Serial.println(job.studentName);
  Serial.print("[ENROLL] Assigned slot: #"); Serial.println(job.assignedSensorFingerprintId);

  updateLcdOpLine("Enrolling...");
  readyToScanShown = false;
  autoScanAwaitingFingerRemoval = false;

  activeSlotId = job.assignedSensorFingerprintId;
  uint8_t enrollResult = getFingerprintEnroll();
  if (enrollResult != FINGERPRINT_OK) {
    String failureReason = enrollmentFailureMessage(enrollResult);
    Serial.print("[ENROLL] Failed: ");
    Serial.println(failureReason);
    lcdFlashResult("Enroll failed");
    deliverEnrollmentResult(job, false, "", failureReason);
    autoScanAwaitingFingerRemoval = true;
    showReadyToScan();
    return;
  }

  String templateBase64;
  if (!exportTemplateBase64(job.assignedSensorFingerprintId, templateBase64)) {
    Serial.println("[ENROLL] Failed to export template backup. Rolling back local slot.");
    deleteFingerprint(job.assignedSensorFingerprintId);
    lcdFlashResult("Export failed");
    deliverEnrollmentResult(job, false, "", "Failed to export template backup from sensor");
    autoScanAwaitingFingerRemoval = true;
    showReadyToScan();
    return;
  }

  EnrollmentResultDeliveryStatus deliveryStatus = deliverEnrollmentResult(job, true, templateBase64, "");
  if (deliveryStatus == ENROLLMENT_RESULT_REJECTED) {
    Serial.println("[ENROLL] Backend rejected the enrollment result. Removing the local slot.");
    deleteFingerprint(job.assignedSensorFingerprintId);
    lcdFlashResult("Enroll rejected");
  } else if (deliveryStatus == ENROLLMENT_RESULT_RETRY_LATER) {
    Serial.println("[ENROLL] Enrollment confirmation is pending retry. Keeping the local slot until the backend confirms.");
    lcdFlashResult("Enroll pending...");
  } else {
    lcdFlashResult("Enrolled OK");
  }

  autoScanAwaitingFingerRemoval = true;
  showReadyToScan();
}

void getHealthCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    lcdFlashResult("WiFi not connected");
    return;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!beginApiRequest(http, ENDPOINT_HEALTH)) {
    return;
  }

  Serial.println("\n[GET] " + String(ENDPOINT_HEALTH));
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[GET] Status code : "); Serial.println(httpCode);
    Serial.print("[GET] Response     : "); Serial.println(response);
    lcdFlashResult(httpCode == 200 ? "Server: OK" : "Server: Error");
  } else {
    Serial.print("[GET] Request failed, error: ");
    Serial.println(http.errorToString(httpCode));
    lcdFlashResult("Health check failed");
  }

  http.end();
}

EnrollmentResultDeliveryStatus postEnrollmentResult(const EnrollmentJob& job, bool success, const String& backupTemplateBase64, const String& failureReason) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    return ENROLLMENT_RESULT_RETRY_LATER;
  }

  JsonDocument doc;
  doc["id"] = job.id;
  doc["deviceId"] = DEVICE_ID;
  doc["sensorFingerprintId"] = job.assignedSensorFingerprintId;
  doc["success"] = success;
  if (success) {
    doc["backupTemplateBase64"] = backupTemplateBase64;
  } else {
    doc["failureReason"] = failureReason;
  }

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!beginApiRequest(http, String(BASE_URL) + "/fingerprint/devices/enrollment-result")) {
    return ENROLLMENT_RESULT_RETRY_LATER;
  }
  http.addHeader("Content-Type", "application/json");
  addDeviceHeaders(http);

  Serial.println("\n[POST] Completing enrollment session");
  Serial.println("[POST] Payload: " + payload);

  int httpCode = http.POST(payload);
  if (httpCode <= 0) {
    Serial.print("[POST] Request failed, error: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return ENROLLMENT_RESULT_RETRY_LATER;
  }

  String response = http.getString();
  Serial.print("[POST] Status code : "); Serial.println(httpCode);
  Serial.print("[POST] Response     : "); Serial.println(response);
  http.end();

  if (httpCode == 200 || httpCode == 201 || httpCode == 202 || httpCode == 204 || httpCode == 409) {
    return ENROLLMENT_RESULT_CONFIRMED;
  }

  if (httpCode == 408 || httpCode == 425 || httpCode == 429 || httpCode >= 500) {
    return ENROLLMENT_RESULT_RETRY_LATER;
  }

  return ENROLLMENT_RESULT_REJECTED;
}

void postScanResult(uint16_t fingerprintID, uint16_t confidence) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected.");
    return;
  }

  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["sensorFingerprintId"] = fingerprintID;
  doc["confidence"] = confidence;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!beginApiRequest(http, String(BASE_URL) + "/fingerprint/devices/scan")) {
    return;
  }
  http.addHeader("Content-Type", "application/json");
  addDeviceHeaders(http);

  Serial.println("\n[POST] " + String(BASE_URL) + "/fingerprint/devices/scan");
  Serial.println("[POST] Payload: " + payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[POST] Status code : "); Serial.println(httpCode);
    Serial.print("[POST] Response     : "); Serial.println(response);

    // Parse response and display message on LCD
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (!error && doc.containsKey("message")) {
      String message = String((const char*)(doc["message"] | ""));
      if (message.length() > 0) {
        lcdFlashResult(message.c_str());
      }
    }
  } else {
    Serial.print("[POST] Request failed, error: ");
    Serial.println(http.errorToString(httpCode));
    lcdFlashResult("Scan failed");
  }

  http.end();
}

uint8_t getFingerprintEnroll() {
  int p = -1;

  Serial.print("\nWaiting for finger to enroll as slot #"); Serial.println(activeSlotId);
  lcdPrintLine(3, "Place finger...");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) Serial.println("Image captured.");
    else if (p == FINGERPRINT_NOFINGER) Serial.print(".");
    else if (p == FINGERPRINT_IMAGEFAIL) Serial.println("[ERROR] Imaging error.");
    else Serial.println("[ERROR] Communication error.");
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    lcdFlashResult("Convert failed");
    return p;
  }

  Serial.println("Remove finger...");
  lcdPrintLine(3, "Remove finger...");
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }

  Serial.println("Place the SAME finger again...");
  lcdPrintLine(3, "Place same finger");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) Serial.println("Image captured.");
    else if (p == FINGERPRINT_NOFINGER) Serial.print(".");
    else Serial.println("[ERROR] Communication error.");
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    lcdFlashResult("Convert failed");
    return p;
  }

  p = finger.createModel();
  if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("[ERROR] Fingerprints did not match. Try again.");
    lcdFlashResult("No match");
    return p;
  }
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not create model.");
    lcdFlashResult("Model failed");
    return p;
  }

  p = finger.storeModel(activeSlotId);
  if (p == FINGERPRINT_OK) {
    Serial.print("[SUCCESS] Fingerprint stored as slot #");
    Serial.println(activeSlotId);
  } else {
    Serial.println("[ERROR] Failed to store fingerprint.");
  }

  return p;
}

bool getFingerprintID(bool waitForFinger) {
  int p = -1;
  bool imageCaptured = false;
  if (waitForFinger) {
    lcdPrintLine(3, "Place finger...");
  }

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      imageCaptured = true;
      Serial.println("\nImage captured.");
    } else if (p == FINGERPRINT_NOFINGER) {
      if (!waitForFinger) {
        return false;
      }

      Serial.print(".");
      delay(50);
    } else if (p == FINGERPRINT_IMAGEFAIL) {
      Serial.println("\n[ERROR] Imaging error.");
      lcdFlashResult("Imaging error");
      return imageCaptured;
    } else {
      Serial.println("\n[ERROR] Communication error.");
      lcdFlashResult("Comm error");
      return imageCaptured;
    }
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.println("[ERROR] Could not convert image.");
    lcdFlashResult("Convert failed");
    return imageCaptured;
  }

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    Serial.println("\n============================");
    Serial.println("  ✔ FINGERPRINT MATCHED");
    Serial.print("  Slot ID   : #"); Serial.println(finger.fingerID);
    Serial.print("  Confidence: "); Serial.println(finger.confidence);
    Serial.println("============================\n");
    String matchMsg = "Match #";
    matchMsg += finger.fingerID;
    matchMsg += " Conf:";
    matchMsg += finger.confidence;
    lcdFlashResult(matchMsg.c_str());
    postScanResult(finger.fingerID, finger.confidence);
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("[SCAN] No matching fingerprint found on sensor.");
    lcdFlashResult("No match found");
  } else {
    Serial.println("[ERROR] Search failed.");
    lcdFlashResult("Search failed");
  }

  return imageCaptured;
}

void deleteFingerprint(uint16_t delID) {
  drainFingerprintSerial(TEMPLATE_STREAM_IDLE_MS, TEMPLATE_STREAM_DRAIN_TIMEOUT_MS);

  uint8_t result = finger.deleteModel(delID);
  if (result == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("[DELETE] Sensor stream was busy. Retrying delete once...");
    drainFingerprintSerial(TEMPLATE_STREAM_IDLE_MS, TEMPLATE_STREAM_DRAIN_TIMEOUT_MS);
    delay(50);
    result = finger.deleteModel(delID);
  }

  if (result == FINGERPRINT_OK) {
    Serial.print("[DELETE] Fingerprint slot #");
    Serial.print(delID);
    Serial.println(" deleted.");
  } else {
    Serial.print("[ERROR] Could not delete fingerprint. Sensor code: ");
    Serial.println(result);
  }
}

void addDeviceHeaders(HTTPClient& http) {
  http.addHeader("X-Device-Api-Key", DEVICE_API_KEY);
}

uint16_t getMaxSensorSlotId() {
  const uint16_t discoveredCapacity = finger.capacity;
  if (discoveredCapacity == 0) {
    return 127;
  }
  return discoveredCapacity;
}

bool isValidSensorSlot(uint16_t slotId, bool logReason) {
  const uint16_t maxSlotId = getMaxSensorSlotId();
  const bool valid = slotId >= 1 && slotId <= maxSlotId;

  if (!valid && logReason) {
    Serial.print("[SLOT] Invalid slot #");
    Serial.print(slotId);
    Serial.print(". Valid range for this sensor is 1-");
    Serial.println(maxSlotId);
  }

  return valid;
}

String invalidSlotFailureMessage(uint16_t slotId) {
  String failure = "Assigned sensor slot ";
  failure += slotId;
  failure += " is outside the valid range 1-";
  failure += getMaxSensorSlotId();
  return failure;
}

void storePendingEnrollmentResult(const EnrollmentJob& job, bool success, const String& backupTemplateBase64, const String& failureReason) {
  pendingEnrollmentResult.active = true;
  pendingEnrollmentResult.job = job;
  pendingEnrollmentResult.success = success;
  pendingEnrollmentResult.backupTemplateBase64 = backupTemplateBase64;
  pendingEnrollmentResult.failureReason = failureReason;
  pendingEnrollmentResult.nextAttemptAtMs = millis() + ENROLLMENT_RESULT_RETRY_INTERVAL_MS;
  pendingEnrollmentResult.attemptCount = 1;
}

void clearPendingEnrollmentResult() {
  pendingEnrollmentResult.active = false;
  pendingEnrollmentResult.backupTemplateBase64 = "";
  pendingEnrollmentResult.failureReason = "";
  pendingEnrollmentResult.nextAttemptAtMs = 0;
  pendingEnrollmentResult.attemptCount = 0;
}

EnrollmentResultDeliveryStatus deliverEnrollmentResult(const EnrollmentJob& job, bool success, const String& backupTemplateBase64, const String& failureReason) {
  EnrollmentResultDeliveryStatus status = postEnrollmentResult(job, success, backupTemplateBase64, failureReason);
  if (status == ENROLLMENT_RESULT_RETRY_LATER) {
    storePendingEnrollmentResult(job, success, backupTemplateBase64, failureReason);
  }
  return status;
}

void processPendingEnrollmentResultIfDue() {
  if (!pendingEnrollmentResult.active) {
    return;
  }

  if (millis() < pendingEnrollmentResult.nextAttemptAtMs) {
    return;
  }

  Serial.print("[POST] Retrying enrollment confirmation, attempt #");
  Serial.println(pendingEnrollmentResult.attemptCount + 1);

  EnrollmentResultDeliveryStatus status = postEnrollmentResult(
    pendingEnrollmentResult.job,
    pendingEnrollmentResult.success,
    pendingEnrollmentResult.backupTemplateBase64,
    pendingEnrollmentResult.failureReason
  );

  if (status == ENROLLMENT_RESULT_CONFIRMED) {
    Serial.println("[POST] Backend confirmed the deferred enrollment result.");
    lcdFlashResult("Enroll confirmed");
    clearPendingEnrollmentResult();
    return;
  }

  if (status == ENROLLMENT_RESULT_REJECTED) {
    Serial.println("[POST] Backend permanently rejected the deferred enrollment result.");
    lcdFlashResult("Enroll rejected");
    if (pendingEnrollmentResult.success) {
      Serial.println("[POST] Removing the local fingerprint because the backend explicitly rejected the enrollment.");
      deleteFingerprint(pendingEnrollmentResult.job.assignedSensorFingerprintId);
    }
    clearPendingEnrollmentResult();
    return;
  }

  pendingEnrollmentResult.attemptCount++;
  pendingEnrollmentResult.nextAttemptAtMs = millis() + ENROLLMENT_RESULT_RETRY_INTERVAL_MS;
}

String enrollmentFailureMessage(uint8_t code) {
  switch (code) {
    case FINGERPRINT_OK: return "Enrollment succeeded";
    case FINGERPRINT_PACKETRECIEVEERR: return "Communication error with fingerprint sensor";
    case FINGERPRINT_IMAGEFAIL: return "Fingerprint imaging failed";
    case FINGERPRINT_IMAGEMESS: return "Fingerprint image was too messy";
    case FINGERPRINT_FEATUREFAIL: return "Failed to extract fingerprint features";
    case FINGERPRINT_INVALIDIMAGE: return "Fingerprint image was invalid";
    case FINGERPRINT_ENROLLMISMATCH: return "The two scans did not match";
    case FINGERPRINT_BADLOCATION: return "Invalid sensor slot";
    case FINGERPRINT_FLASHERR: return "Sensor flash write failed";
    default: return "Unknown enrollment failure";
  }
}

bool exportTemplateBase64(uint16_t slotId, String& outBase64) {
  drainFingerprintSerial(TEMPLATE_STREAM_IDLE_MS, TEMPLATE_STREAM_DRAIN_TIMEOUT_MS);

  if (finger.loadModel(slotId) != FINGERPRINT_OK) {
    Serial.println("[EXPORT] Failed to load stored model into sensor buffer.");
    return false;
  }

  if (finger.getModel() != FINGERPRINT_OK) {
    Serial.println("[EXPORT] Sensor refused template upload command.");
    return false;
  }

  size_t bufferCapacity = TEMPLATE_INITIAL_BUFFER_SIZE;
  uint8_t* templateBytes = static_cast<uint8_t*>(malloc(bufferCapacity));
  if (templateBytes == nullptr) {
    Serial.println("[EXPORT] Failed to allocate template buffer.");
    drainFingerprintSerial(TEMPLATE_STREAM_IDLE_MS, TEMPLATE_STREAM_DRAIN_TIMEOUT_MS);
    return false;
  }

  size_t totalBytes = 0;
  bool exportComplete = false;

  while (true) {
    uint8_t packetType = 0;
    uint8_t packetPayload[TEMPLATE_PACKET_BUFFER_SIZE];
    size_t packetPayloadLength = 0;

    if (!readFingerprintDataPacket(packetType, packetPayload, sizeof(packetPayload), packetPayloadLength)) {
      Serial.println("[EXPORT] Failed to read template packet from sensor.");
      break;
    }

    if (totalBytes + packetPayloadLength > bufferCapacity) {
      size_t newCapacity = bufferCapacity;
      while (newCapacity < totalBytes + packetPayloadLength && newCapacity < TEMPLATE_MAX_BUFFER_SIZE) {
        newCapacity *= 2;
      }

      if (newCapacity < totalBytes + packetPayloadLength || newCapacity > TEMPLATE_MAX_BUFFER_SIZE) {
        Serial.print("[EXPORT] Template exceeded maximum buffer size of ");
        Serial.print(TEMPLATE_MAX_BUFFER_SIZE);
        Serial.println(" bytes.");
        break;
      }

      uint8_t* resizedBuffer = static_cast<uint8_t*>(realloc(templateBytes, newCapacity));
      if (resizedBuffer == nullptr) {
        Serial.println("[EXPORT] Failed to grow template buffer.");
        break;
      }

      templateBytes = resizedBuffer;
      bufferCapacity = newCapacity;
    }

    memcpy(templateBytes + totalBytes, packetPayload, packetPayloadLength);
    totalBytes += packetPayloadLength;

    if (packetType == FINGERPRINT_ENDDATAPACKET) {
      exportComplete = true;
      break;
    }

    if (packetType != FINGERPRINT_DATAPACKET) {
      Serial.println("[EXPORT] Unexpected packet type while exporting template.");
      break;
    }
  }

  drainFingerprintSerial(TEMPLATE_STREAM_IDLE_MS, TEMPLATE_STREAM_DRAIN_TIMEOUT_MS);

  bool encoded = false;
  if (exportComplete) {
    Serial.print("[EXPORT] Template bytes captured: ");
    Serial.println(totalBytes);
    encoded = base64Encode(templateBytes, totalBytes, outBase64);
  }

  free(templateBytes);
  return exportComplete && encoded;
}

bool readFingerprintDataPacket(uint8_t& packetType, uint8_t* payloadBuffer, size_t payloadCapacity, size_t& payloadLength) {
  uint8_t header[9];
  if (!readExactBytes(header, sizeof(header), TEMPLATE_EXPORT_TIMEOUT_MS)) {
    return false;
  }

  uint16_t startCode = (static_cast<uint16_t>(header[0]) << 8) | header[1];
  if (startCode != FINGERPRINT_STARTCODE) {
    Serial.println("[EXPORT] Invalid packet start code.");
    return false;
  }

  packetType = header[6];
  uint16_t wireLength = (static_cast<uint16_t>(header[7]) << 8) | header[8];
  if (wireLength < 2) {
    Serial.println("[EXPORT] Invalid packet length.");
    return false;
  }

  payloadLength = wireLength - 2;
  if (payloadLength > payloadCapacity) {
    Serial.println("[EXPORT] Packet payload exceeds local buffer.");
    return false;
  }

  if (!readExactBytes(payloadBuffer, payloadLength, TEMPLATE_EXPORT_TIMEOUT_MS)) {
    return false;
  }

  uint8_t checksum[2];
  if (!readExactBytes(checksum, sizeof(checksum), TEMPLATE_EXPORT_TIMEOUT_MS)) {
    return false;
  }

  return true;
}

bool readExactBytes(uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  size_t bytesRead = 0;
  const unsigned long startedAt = millis();

  while (bytesRead < length) {
    if (fingerSerial.available()) {
      int nextByte = fingerSerial.read();
      if (nextByte >= 0) {
        buffer[bytesRead++] = static_cast<uint8_t>(nextByte);
      }
      continue;
    }

    if (millis() - startedAt >= timeoutMs) {
      Serial.println("[EXPORT] Timed out reading template bytes from sensor.");
      return false;
    }

    delay(1);
  }

  return true;
}

bool drainFingerprintSerial(uint32_t idleMs, uint32_t maxDurationMs) {
  const unsigned long startedAt = millis();
  unsigned long lastByteAt = millis();
  size_t drainedBytes = 0;

  while (millis() - startedAt < maxDurationMs) {
    bool consumedByte = false;
    while (fingerSerial.available()) {
      fingerSerial.read();
      drainedBytes++;
      lastByteAt = millis();
      consumedByte = true;
    }

    if (!consumedByte && millis() - lastByteAt >= idleMs) {
      break;
    }

    delay(1);
  }

  if (drainedBytes > 0) {
    Serial.print("[EXPORT] Drained ");
    Serial.print(drainedBytes);
    Serial.println(" residual bytes from sensor stream.");
  }

  return true;
}

bool base64Encode(const uint8_t* data, size_t length, String& outBase64) {
  size_t outputLength = 0;
  const size_t outputCapacity = ((length + 2) / 3) * 4 + 4;
  unsigned char* encodedBuffer = static_cast<unsigned char*>(malloc(outputCapacity));
  if (encodedBuffer == nullptr) {
    Serial.println("[EXPORT] Failed to allocate base64 buffer.");
    return false;
  }

  int result = mbedtls_base64_encode(encodedBuffer, outputCapacity, &outputLength, data, length);
  if (result != 0) {
    free(encodedBuffer);
    Serial.print("[EXPORT] Base64 encoding failed with code ");
    Serial.println(result);
    return false;
  }

  encodedBuffer[outputLength] = '\0';
  outBase64 = String(reinterpret_cast<char*>(encodedBuffer));
  free(encodedBuffer);
  return true;
}

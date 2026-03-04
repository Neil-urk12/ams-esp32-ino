#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>

// ── Pin Definitions ──────────────────────────────────────────────
#define FP_RX_PIN 16
#define FP_TX_PIN 17

HardwareSerial fingerSerial(2);  // UART2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

uint8_t id;

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial); delay(100);

  Serial.println("\n\n=== ESP32 Fingerprint Attendance System ===");

  // finger.begin() internally calls mySerial.begin() — no need for Serial2.begin()
  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);

  if (finger.verifyPassword()) {
    Serial.println("[OK] Fingerprint sensor found!");
  } else {
    Serial.println("[ERROR] Fingerprint sensor not found. Check wiring.");
    while (1) { delay(1); }
  }

  // Print sensor parameters
  finger.getParameters();
  Serial.println(F("\n--- Sensor Parameters ---"));
  Serial.print(F("  Status       : 0x")); Serial.println(finger.status_reg, HEX);
  Serial.print(F("  System ID    : 0x")); Serial.println(finger.system_id, HEX);
  Serial.print(F("  Capacity     : "));   Serial.println(finger.capacity);
  Serial.print(F("  Security Lvl : "));   Serial.println(finger.security_level);
  Serial.print(F("  Baud Rate    : "));   Serial.println(finger.baud_rate);

  // Show how many fingerprints are stored
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
        Serial.print("Patapim")
        printMenu();
        break;

      case 'H':
        Serial.print("Health check endpoint")
        printMenu();
        break;

      case ''
    }
  }
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
    Serial.print("  ID       : #"); Serial.println(finger.fingerID);
    Serial.print("  Confidence: "); Serial.println(finger.confidence);
    Serial.println("  [ATTENDANCE LOGGED]");
    Serial.println("============================\n");
    // TODO: Send attendance record to your backend (FastAPI/Supabase) via WiFi here
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

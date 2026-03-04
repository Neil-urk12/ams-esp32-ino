#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>

HardwareSerial fingerSerial(2);  // UART2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

void setup() {
  Serial.begin(115200);
  while (!Serial); delay(100);
  Serial.println("Fingerprint sensor test");

  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);  // Critical: baud, config, RX=16, TX=17
  if (finger.verifyPassword()) {
    Serial.println("Found sensor!");
    uint8_t status = finger.status_reg;
    Serial.print("Status: 0x"); Serial.println(status, HEX);
    finger.getTemplateCount(); Serial.print("Templates: "); Serial.println(finger.templateCount);
  } else {
    Serial.println("Sensor not found - check wiring/code");
    while(1);
  }
}

void loop() {}

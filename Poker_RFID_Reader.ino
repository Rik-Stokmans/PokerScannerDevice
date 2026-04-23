#include <SPI.h>
#include <MFRC522.h>
#include <NimBLEDevice.h>
#include <string>   // For std::string

// Define pins for RC522 modules
#define SS_PIN1 3   // CS for Reader 1 (D2, GPIO2)
#define RST_PIN1 5  // RST for Reader 1 (D4, GPIO4)
#define SS_PIN2 4   // CS for Reader 2 (D3, GPIO3)
#define RST_PIN2 6  // RST for Reader 2 (D5, GPIO5)
#define BATTERY_PIN 2 // Battery voltage sense (D0, GPIO0)

// Create MFRC522 instances
MFRC522 mfrc522_1(SS_PIN1, RST_PIN1); // Reader 1
MFRC522 mfrc522_2(SS_PIN2, RST_PIN2); // Reader 2

// BLE Service and Characteristic UUIDs
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define RFID_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BATTERY_CHARACTERISTIC_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

// NimBLE globals
NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pRfidCharacteristic = nullptr;
NimBLECharacteristic *pBatteryCharacteristic = nullptr;
bool deviceConnected = false;
bool uidDetected = false; // Track if card was detected

// BLE Server Callbacks
class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
      deviceConnected = true;
      Serial.println("BLE Connected");
      NimBLEDevice::getAdvertising()->stop();
    }
    void onDisconnect(NimBLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE Disconnected");
      NimBLEDevice::getAdvertising()->start();
    }
};

unsigned long lastScanTime = 0;
const unsigned long scanInterval = 5000; // 5s scan interval

void setup() {
  // Initialize Serial for debugging (disable in production)
  Serial.begin(9600);
  Serial.println("Starting...");

  // Initialize SPI
  SPI.begin(8, 9, 10); // SCK=D8, MISO=D9, MOSI=D10
  delay(10); // SPI stabilization
  Serial.printf("Free Heap after SPI: %u B\n", ESP.getFreeHeap());

  // Initialize RST pins and power up RFID
  pinMode(RST_PIN1, OUTPUT);
  pinMode(RST_PIN2, OUTPUT);
  powerUpRFID();
  Serial.println("RFID Initialized");

  // Initialize battery pin
  pinMode(BATTERY_PIN, INPUT);

  // Delay to ensure FreeRTOS stability
  delay(100);
  Serial.printf("Free Heap before NimBLE: %u B\n", ESP.getFreeHeap());

  // Initialize NimBLE (init must come before any other NimBLE calls)
  NimBLEDevice::init("RFID Scanner");
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC); // Public address
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm TX power
  Serial.println("NimBLE Initialized");
  Serial.printf("Free Heap after NimBLE init: %u B\n", ESP.getFreeHeap());

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  Serial.println("BLE Server Created");

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pRfidCharacteristic = pService->createCharacteristic(
                          RFID_CHARACTERISTIC_UUID,
                          NIMBLE_PROPERTY::READ |
                          NIMBLE_PROPERTY::WRITE |
                          NIMBLE_PROPERTY::NOTIFY
                        );
  Serial.println("RFID Characteristic Created");

  pBatteryCharacteristic = pService->createCharacteristic(
                            BATTERY_CHARACTERISTIC_UUID,
                            NIMBLE_PROPERTY::READ |
                            NIMBLE_PROPERTY::NOTIFY
                          );
  Serial.println("Battery Characteristic Created");

  pService->start();
  Serial.println("BLE Service Started");
  Serial.printf("Free Heap after service start: %u B\n", ESP.getFreeHeap());

  // Start advertising
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setMinInterval(3200); // 2000ms interval
  pAdvertising->setMaxInterval(3200);
  bool advStarted = pAdvertising->start();
  Serial.printf("Advertising Started: %s\n", advStarted ? "Success" : "Failed");
}

void loop() {
  if (millis() - lastScanTime >= scanInterval) {
    powerUpRFID();
    uidDetected = false;
    for (int i = 0; i < 5; i++) {
      mfrc522_1.PCD_Init(); // Re-init to clear any lingering SPI/RF state
      checkReader(mfrc522_1, "R1");
      delay(20); // Allow RF field to settle before switching to reader 2
      mfrc522_2.PCD_Init(); // Re-init to clear any lingering SPI/RF state
      checkReader(mfrc522_2, "R2");
      if (i != 4) delay(20);
    }
    powerDownRFID();
    if (uidDetected && deviceConnected) {
      sendBatteryPercentage();
    }
    lastScanTime = millis();

    Serial.printf("Free Heap: %u B, Connected: %d, UID Detected: %d\n", ESP.getFreeHeap(), deviceConnected, uidDetected);
  }

  delay(100); // Allow CPU to idle or enter light sleep
}

void checkReader(MFRC522 &reader, const char* readerName) {
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) {
    return;
  }

  char uidStr[32];
  snprintf(uidStr, sizeof(uidStr), "%s: ", readerName);
  int pos = strlen(uidStr);
  for (byte i = 0; i < reader.uid.size; i++) {
    snprintf(uidStr + pos, sizeof(uidStr) - pos, "%s%02X", i ? " " : "", reader.uid.uidByte[i]);
    pos = strlen(uidStr);
  }

  if (deviceConnected) {
    std::string uidValue = uidStr;
    pRfidCharacteristic->setValue(uidValue);
    pRfidCharacteristic->notify();
    delay(100); // Ensure notification sent
    Serial.println("Sent UID: " + String(uidStr));
  }
  uidDetected = true;
  reader.PICC_HaltA();
  reader.PCD_StopCrypto1(); // Clear crypto state so the reader is ready for the next card
}

void sendBatteryPercentage() {
  int adcValue = analogRead(BATTERY_PIN);
  float voltage = (adcValue / 4095.0) * 3.3 * 2;
  int percentage = (voltage - 3.0) / (4.2 - 3.0) * 100;

  char battStr[16];
  snprintf(battStr, sizeof(battStr), "BAT: %d%%", percentage);

  if (deviceConnected) {
    std::string battValue = battStr;
    pBatteryCharacteristic->setValue(battValue);
    pBatteryCharacteristic->notify();
    delay(100); // Ensure notification sent
    Serial.println("Sent Battery: " + String(battStr));
  }
}

void powerUpRFID() {
  digitalWrite(RST_PIN1, HIGH);
  digitalWrite(RST_PIN2, HIGH);
  delay(10);
  mfrc522_1.PCD_Init();
  mfrc522_2.PCD_Init();
}

void powerDownRFID() {
  digitalWrite(RST_PIN1, LOW);
  digitalWrite(RST_PIN2, LOW);
}
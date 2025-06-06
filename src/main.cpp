#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>

// Service and Characteristic UUIDs for counter synchronization
#define SERVICE_UUID "21e862dc-87da-4130-9991-2a5a49b4d949"
#define COUNTER_CHARACTERISTIC_UUID "4027ce63-bdf0-4158-9426-6c8203185e00"
#define SYNC_CHARACTERISTIC_UUID "e0368f9c-d3d2-4588-b033-1355ac7dc562"

// Timing constants
#define COUNTER_INTERVAL 1000  // Increment counter every 1 second
#define SYNC_INTERVAL 5000     // Sync every 5 seconds
#define SCAN_TIME 3            // Scan for 3 seconds

// Global variables
uint32_t localCounter = 0;
uint32_t remoteCounter = 0;
unsigned long lastCounterUpdate = 0;
unsigned long lastSyncTime = 0;
String deviceName;

// BLE Server components
BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pCounterCharacteristic = nullptr;
BLECharacteristic* pSyncCharacteristic = nullptr;

// BLE Client components
BLEClient* pClient = nullptr;
BLERemoteService* pRemoteService = nullptr;
BLERemoteCharacteristic* pRemoteCounterCharacteristic = nullptr;
BLERemoteCharacteristic* pRemoteSyncCharacteristic = nullptr;

// Connection states
bool serverConnected = false;
bool clientConnected = false;
bool doConnect = false;
bool doScan = false;
BLEAdvertisedDevice* targetDevice = nullptr;

// Server callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      serverConnected = true;
      Serial.println("Server: Client connected");
    };

    void onDisconnect(BLEServer* pServer) {
      serverConnected = false;
      Serial.println("Server: Client disconnected");
      pServer->startAdvertising(); // Restart advertising
    }
};

// Client callbacks
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    Serial.println("Client: Connected to server");
  }

  void onDisconnect(BLEClient* pclient) {
    clientConnected = false;
    Serial.println("Client: Disconnected from server");
  }
};

// Sync characteristic callback
class MySyncCallback: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      if (value.length() == 4) {
        uint32_t receivedCounter;
        memcpy(&receivedCounter, value.data(), 4);
        
        // Sync logic: use the higher counter value
        if (receivedCounter > localCounter) {
          localCounter = receivedCounter;
          Serial.printf("Sync: Updated local counter to %d\n", localCounter);
        }
      }
    }
};

// Advertised device scanner
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      if (advertisedDevice.haveServiceUUID() && 
          advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
        
        // Found a device with our service
        Serial.printf("Found target device: %s\n", advertisedDevice.getAddress().toString().c_str());
        BLEDevice::getScan()->stop();
        targetDevice = new BLEAdvertisedDevice(advertisedDevice);
        doConnect = true;
        doScan = false;
      }
    }
};

void setupBLEServer() {
  // Create server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create service
  pService = pServer->createService(SERVICE_UUID);
  
  // Create counter characteristic (read/notify)
  pCounterCharacteristic = pService->createCharacteristic(
    COUNTER_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  
  // Create sync characteristic (read/write)
  pSyncCharacteristic = pService->createCharacteristic(
    SYNC_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  
  // Set sync callback
  pSyncCharacteristic->setCallbacks(new MySyncCallback());
  
  // Add descriptors for notifications
  pCounterCharacteristic->addDescriptor(new BLE2902());
  
  // Start service
  pService->start();
  
  // Start advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("BLE Server started and advertising");
}

void setupBLEClient() {
  // Set up scanner
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
}

bool connectToServer() {
  Serial.printf("Attempting to connect to %s\n", targetDevice->getAddress().toString().c_str());
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  
  // Connect to remote BLE server
  pClient->connect(targetDevice);
  Serial.println("Connected to server");
  
  // Obtain reference to service
  pRemoteService = pClient->getService(SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find service UUID");
    pClient->disconnect();
    return false;
  }
  Serial.println("Found service");
  
  // Obtain references to characteristics
  pRemoteCounterCharacteristic = pRemoteService->getCharacteristic(COUNTER_CHARACTERISTIC_UUID);
  pRemoteSyncCharacteristic = pRemoteService->getCharacteristic(SYNC_CHARACTERISTIC_UUID);
  
  if (pRemoteCounterCharacteristic == nullptr || pRemoteSyncCharacteristic == nullptr) {
    Serial.println("Failed to find characteristics");
    pClient->disconnect();
    return false;
  }
  Serial.println("Found characteristics");
  
  clientConnected = true;
  return true;
}

void performSync() {
  if (clientConnected && pRemoteSyncCharacteristic != nullptr) {
    // Read remote counter
    std::string value = pRemoteCounterCharacteristic->readValue();
    if (value.length() == 4) {
      memcpy(&remoteCounter, value.data(), 4);
      Serial.printf("Remote counter: %d, Local counter: %d\n", remoteCounter, localCounter);
      
      // Determine which counter is higher and sync
      uint32_t syncValue = max(localCounter, remoteCounter);
      if (syncValue != localCounter) {
        localCounter = syncValue;
        Serial.printf("Synced local counter to %d\n", localCounter);
      }
      
      // Write our counter to remote sync characteristic
      pRemoteSyncCharacteristic->writeValue((uint8_t*)&localCounter, 4);
    }
  }
}

void updateCounter() {
  localCounter++;
  Serial.printf("Local counter: %d\n", localCounter);
  
  // Update server characteristic
  pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
  if (serverConnected) {
    pCounterCharacteristic->notify();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Generate unique device name based on MAC address
  uint64_t chipid = ESP.getEfuseMac();
  deviceName = "ESP32Counter_" + String((uint16_t)(chipid >> 32), HEX);
  
  Serial.printf("Starting %s...\n", deviceName.c_str());
  
  // Initialize BLE
  BLEDevice::init(deviceName.c_str());
  
  // Setup server and client
  setupBLEServer();
  setupBLEClient();
  
  // Start scanning for other devices
  doScan = true;
  
  Serial.println("Setup complete!");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Update counter every second
  if (currentTime - lastCounterUpdate >= COUNTER_INTERVAL) {
    updateCounter();
    lastCounterUpdate = currentTime;
  }
  
  // Perform sync every 5 seconds
  if (currentTime - lastSyncTime >= SYNC_INTERVAL) {
    if (clientConnected) {
      performSync();
    }
    lastSyncTime = currentTime;
  }
  
  // Handle scanning and connection
  if (doScan) {
    BLEDevice::getScan()->start(SCAN_TIME, false);
    doScan = false;
  }
  
  // Connect to discovered device
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Successfully connected to server");
    } else {
      Serial.println("Failed to connect to server");
    }
    doConnect = false;
  }
  
  // If not connected as client and not currently scanning, start scanning again
  if (!clientConnected && !doScan && !doConnect) {
    Serial.println("Not connected, starting scan...");
    doScan = true;
    delay(1000); // Wait a bit before scanning again
  }
  
  delay(100); // Small delay to prevent overwhelming the system
}
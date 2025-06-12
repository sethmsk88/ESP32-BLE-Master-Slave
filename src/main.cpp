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
#define TIMESTAMP_CHARACTERISTIC_UUID "f0368f9c-d3d2-4588-b033-1355ac7dc563"

// Timing constants
#define COUNTER_INTERVAL 1000  // Increment counter every 1 second
#define SYNC_INTERVAL 5000     // Sync every 5 seconds
#define SCAN_TIME 3            // Scan for 3 seconds

// Global variables
uint32_t localCounter = 0;
uint32_t remoteCounter = 0;
unsigned long lastCounterUpdate = 0;
unsigned long lastSyncTime = 0;
unsigned long lastScanAttempt = 0;
unsigned long bootTimestamp = 0;
String deviceName;

// Role management
bool isMaster = false;
bool isClient = false;
bool roleAssigned = false;

// BLE Server components
BLEServer* pServer = nullptr;
BLEService* pService = nullptr;
BLECharacteristic* pCounterCharacteristic = nullptr;
BLECharacteristic* pSyncCharacteristic = nullptr;
BLECharacteristic* pTimestampCharacteristic = nullptr;

// BLE Client components
BLEClient* pClient = nullptr;
BLERemoteService* pRemoteService = nullptr;
BLERemoteCharacteristic* pRemoteCounterCharacteristic = nullptr;
BLERemoteCharacteristic* pRemoteSyncCharacteristic = nullptr;
BLERemoteCharacteristic* pRemoteTimestampCharacteristic = nullptr;

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
      
      // Reset role assignment when server connection is lost
      // This allows for role renegotiation on reconnection
      if (roleAssigned) {
        Serial.println("Server: Resetting role assignment due to disconnection");
        roleAssigned = false;
        isMaster = false;
        isClient = false;
      }
      
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
    
    // Reset role assignment when client connection is lost
    // This allows for role renegotiation on reconnection
    if (roleAssigned) {
      Serial.println("Client: Resetting role assignment due to disconnection");
      roleAssigned = false;
      isMaster = false;
      isClient = false;
    }
    
    // Clean up client connection
    if (pClient != nullptr) {
      pClient = nullptr;
    }
    pRemoteService = nullptr;
    pRemoteCounterCharacteristic = nullptr;
    pRemoteSyncCharacteristic = nullptr;
    pRemoteTimestampCharacteristic = nullptr;
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
        
        // Only connect if we're not already connected as a client
        if (!clientConnected) {
          BLEDevice::getScan()->stop();
          targetDevice = new BLEAdvertisedDevice(advertisedDevice);
          doConnect = true;
          doScan = false;
        } else {
          Serial.println("Already connected as client, ignoring found device");
        }
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
  
  // Create timestamp characteristic (read) - contains boot timestamp for role negotiation
  pTimestampCharacteristic = pService->createCharacteristic(
    TIMESTAMP_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  
  // Set sync callback
  pSyncCharacteristic->setCallbacks(new MySyncCallback());
  
  // Set boot timestamp in characteristic
  pTimestampCharacteristic->setValue((uint8_t*)&bootTimestamp, 4);
  
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
  
  // Set scan result callback to track when scans complete
  Serial.println("BLE Client scanner configured");
}

bool connectToServer() {
  Serial.printf("Attempting to connect to %s\n", targetDevice->getAddress().toString().c_str());
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  
  // Connect to remote BLE server with timeout handling
  if (!pClient->connect(targetDevice)) {
    Serial.println("Failed to connect to server");
    delete pClient;
    pClient = nullptr;
    return false;
  }
  Serial.println("Connected to server");
  
  // Obtain reference to service
  pRemoteService = pClient->getService(SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find service UUID");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    return false;
  }
  Serial.println("Found service");
  
  // Obtain references to characteristics
  pRemoteCounterCharacteristic = pRemoteService->getCharacteristic(COUNTER_CHARACTERISTIC_UUID);
  pRemoteSyncCharacteristic = pRemoteService->getCharacteristic(SYNC_CHARACTERISTIC_UUID);
  pRemoteTimestampCharacteristic = pRemoteService->getCharacteristic(TIMESTAMP_CHARACTERISTIC_UUID);
    if (pRemoteCounterCharacteristic == nullptr || pRemoteSyncCharacteristic == nullptr || pRemoteTimestampCharacteristic == nullptr) {
    Serial.println("Failed to find characteristics");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    return false;
  }
  Serial.println("Found characteristics");
    // Perform role assignment based on boot timestamps
  // Always perform role assignment on each connection to handle resets
  std::string remoteTimestampData = pRemoteTimestampCharacteristic->readValue();
  if (remoteTimestampData.length() == 4) {
    uint32_t remoteBootTimestamp;
    memcpy(&remoteBootTimestamp, remoteTimestampData.data(), 4);
    
    Serial.printf("Local boot timestamp: %lu\n", bootTimestamp);
    Serial.printf("Remote boot timestamp: %lu\n", remoteBootTimestamp);
    
    // Device with earlier (lower) boot timestamp becomes master
    if (bootTimestamp < remoteBootTimestamp) {
      isMaster = true;
      isClient = false;
      Serial.println("ROLE: This device is MASTER (earlier boot time)");
    } else if (bootTimestamp > remoteBootTimestamp) {
      isMaster = false;
      isClient = true;
      Serial.println("ROLE: This device is CLIENT (later boot time)");
    } else {
      // Same timestamp (very unlikely), use MAC address as tiebreaker
      String localMac = BLEDevice::getAddress().toString().c_str();
      String remoteMac = targetDevice->getAddress().toString().c_str();
      if (localMac < remoteMac) {
        isMaster = true;
        isClient = false;
        Serial.println("ROLE: This device is MASTER (MAC address tiebreaker)");
      } else {
        isMaster = false;
        isClient = true;
        Serial.println("ROLE: This device is CLIENT (MAC address tiebreaker)");
      }
    }    roleAssigned = true;
  } else {
    Serial.println("Failed to read remote timestamp");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    return false;
  }
  
  clientConnected = true;
  return true;
}

void performSync() {
  if (clientConnected && pRemoteSyncCharacteristic != nullptr && roleAssigned) {
    if (isMaster) {
      // Master behavior: read remote counter and update it if necessary
      std::string value = pRemoteCounterCharacteristic->readValue();
      if (value.length() == 4) {
        memcpy(&remoteCounter, value.data(), 4);
        Serial.printf("Master sync - Remote: %d, Local: %d\n", remoteCounter, localCounter);
        
        // Master dictates the counter value - use the higher one
        uint32_t syncValue = max(localCounter, remoteCounter);
        if (syncValue != remoteCounter) {
          // Write the sync value to remote device
          pRemoteSyncCharacteristic->writeValue((uint8_t*)&syncValue, 4);
          Serial.printf("Master: Synced remote counter to %d\n", syncValue);
        }
        
        // Update local counter if remote was higher
        if (syncValue != localCounter) {
          localCounter = syncValue;
          Serial.printf("Master: Updated local counter to %d\n", localCounter);
          // Update server characteristic
          pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
          if (serverConnected) {
            pCounterCharacteristic->notify();
          }
        }
      }
    } else if (isClient) {
      // Client behavior: just read master's counter and report
      std::string value = pRemoteCounterCharacteristic->readValue();
      if (value.length() == 4) {
        memcpy(&remoteCounter, value.data(), 4);
        Serial.printf("Client sync - Master counter: %d, Local counter: %d\n", remoteCounter, localCounter);
        
        // Client accepts master's counter value if different
        if (remoteCounter != localCounter) {
          localCounter = remoteCounter;
          Serial.printf("Client: Synchronized to master counter %d\n", localCounter);
          // Update server characteristic
          pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
          if (serverConnected) {
            pCounterCharacteristic->notify();
          }
        }
      }
    }
  }
}

void updateCounter() {
  if (!roleAssigned) {
    // If no role assigned but still running, continue counting in standalone mode
    localCounter++;
    Serial.printf("Standalone counter: %d\n", localCounter);
  } else {
    if (isMaster) {
      // Master increments normally
      localCounter++;
      Serial.printf("Master counter: %d\n", localCounter);
    } else if (isClient) {
      // Client also increments its counter - sync will coordinate the values
      localCounter++;
      if (clientConnected) {
        Serial.printf("Client counter (connected): %d\n", localCounter);
      } else {
        Serial.printf("Client counter (standalone): %d\n", localCounter);
      }
    }
  }
  
  // Update server characteristic
  pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
  if (serverConnected) {
    pCounterCharacteristic->notify();
  }
}

void resetConnectionState() {
  // Reset all connection-related state
  clientConnected = false;
  roleAssigned = false;
  isMaster = false;
  isClient = false;
  
  // Clean up client pointers
  if (pClient != nullptr) {
    if (pClient->isConnected()) {
      pClient->disconnect();
    }
    pClient = nullptr;
  }
  
  pRemoteService = nullptr;
  pRemoteCounterCharacteristic = nullptr;
  pRemoteSyncCharacteristic = nullptr;
  pRemoteTimestampCharacteristic = nullptr;
  targetDevice = nullptr;
  
  // Reset flags
  doConnect = false;
  doScan = false;
  
  Serial.println("Connection state reset - ready for reconnection");
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // Keep this one delay as it's needed for serial initialization
  
  // Generate unique device name and boot timestamp
  uint64_t chipid = ESP.getEfuseMac();
  deviceName = "ESP32Counter_" + String((uint16_t)(chipid >> 32), HEX);
  bootTimestamp = millis(); // Initialize boot timestamp
  
  Serial.printf("Starting %s...\n", deviceName.c_str());
  Serial.printf("Boot timestamp: %lu\n", bootTimestamp);
  
  // Initialize BLE
  BLEDevice::init(deviceName.c_str());
  
  // Setup server and client
  setupBLEServer();
  setupBLEClient();
  
  // Start scanning for other devices
  doScan = true;
  lastScanAttempt = millis(); // Initialize scan timing
  
  Serial.println("Setup complete!");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Update counter every second
  if (currentTime - lastCounterUpdate >= COUNTER_INTERVAL) {
    updateCounter();
    lastCounterUpdate = currentTime;
  }
  
  // Perform sync every 5 seconds (only if connected and role assigned)
  if (currentTime - lastSyncTime >= SYNC_INTERVAL) {
    if (clientConnected && roleAssigned) {
      performSync();
    }
    lastSyncTime = currentTime;
  }
  // Handle scanning and connection
  if (doScan) {
    Serial.printf("Starting BLE scan... (ClientConn: %s, ServerConn: %s, RoleAssigned: %s)\n",
                 clientConnected ? "YES" : "NO",
                 serverConnected ? "YES" : "NO", 
                 roleAssigned ? "YES" : "NO");
    
    BLEScanResults foundDevices = BLEDevice::getScan()->start(SCAN_TIME, false);
    int deviceCount = foundDevices.getCount();
    Serial.printf("Scan complete: Found %d devices\n", deviceCount);
    
    // Debug: list all found devices
    for (int i = 0; i < deviceCount; i++) {
      BLEAdvertisedDevice device = foundDevices.getDevice(i);
      if (device.haveServiceUUID()) {
        Serial.printf("Device %d: %s - Has our service: %s\n", 
                     i, 
                     device.getAddress().toString().c_str(),
                     device.isAdvertisingService(BLEUUID(SERVICE_UUID)) ? "YES" : "NO");
      }
    }
    
    doScan = false;
    lastScanAttempt = currentTime;
  }
  // Connect to discovered device
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Successfully connected to server and role assigned");
    } else {
      Serial.println("Failed to connect to server or assign role");
    }
    doConnect = false;
  }
  
  // Print status every 10 seconds
  static unsigned long lastStatusPrint = 0;
  if (currentTime - lastStatusPrint >= 10000) {
    Serial.printf("Status - Role: %s, ClientConn: %s, ServerConn: %s, Counter: %d\n", 
                 roleAssigned ? (isMaster ? "MASTER" : "CLIENT") : "UNASSIGNED",
                 clientConnected ? "YES" : "NO",
                 serverConnected ? "YES" : "NO",
                 localCounter);
    lastStatusPrint = currentTime;
  }
  
  // Small delay to prevent overwhelming the system (non-blocking alternative)
  // Instead of delay(100), we just continue the loop - the timing checks above provide natural throttling
}
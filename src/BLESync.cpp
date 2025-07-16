
#include "BLESync.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#include <stdlib.h>

// Service and Characteristic UUIDs for counter synchronization
#define SERVICE_UUID "21e862dc-87da-4130-9991-2a5a49b4d949"
#define COUNTER_CHARACTERISTIC_UUID "4027ce63-bdf0-4158-9426-6c8203185e00"
#define SYNC_CHARACTERISTIC_UUID "e0368f9c-d3d2-4588-b033-1355ac7dc562"
#define TIMESTAMP_CHARACTERISTIC_UUID "f0368f9c-d3d2-4588-b033-1355ac7dc563"

// Timing constants
#define COUNTER_INTERVAL 3000  // Increment counter every 1 second
#define SYNC_INTERVAL 10000     // Sync every 10 seconds
#define SCAN_TIME 3            // Scan for 3 seconds
#define RESCAN_INTERVAL 10000  // Rescan every 10 seconds if not connected
#define STATUS_PRINT_INTERVAL 20000 // Print status every 20 seconds
#define CONNECTION_TIMEOUT 10000  // 10 second timeout for connection attempts

// Global variables
static uint32_t localCounter = 0;
static uint32_t remoteCounter = 0;
static unsigned long lastCounterUpdate = 0;
static unsigned long lastSyncTime = 0;
static unsigned long lastScanAttempt = 0;
static unsigned long bootTimestamp = 0;
static String deviceName;

// Role management
static bool isMaster = false;
static bool isClient = false;
static bool roleAssigned = false;

// BLE Server components
static BLEServer* pServer = nullptr;
static BLEService* pService = nullptr;
static BLECharacteristic* pCounterCharacteristic = nullptr;
static BLECharacteristic* pSyncCharacteristic = nullptr;
static BLECharacteristic* pTimestampCharacteristic = nullptr;

// BLE Client components
static BLEClient* pClient = nullptr;
static BLERemoteService* pRemoteService = nullptr;
static BLERemoteCharacteristic* pRemoteCounterCharacteristic = nullptr;
static BLERemoteCharacteristic* pRemoteSyncCharacteristic = nullptr;
static BLERemoteCharacteristic* pRemoteTimestampCharacteristic = nullptr;

// Connection states
static bool serverConnected = false;
static bool clientConnected = false;
static bool doConnect = false;
static bool doScan = false;
static bool doRoleNegotiation = false;
static unsigned long connectAttemptStartTime = 0;
static BLEAdvertisedDevice* targetDevice = nullptr;

// Add a random delay (0-1000ms) before scanning/connecting after disconnect
static unsigned long randomScanDelay = 0;
static unsigned long scanDelayStart = 0;

// Server callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      serverConnected = true;
      Serial.println("Server: Client connected");
      uint32_t currentUptime = millis();
      pTimestampCharacteristic->setValue((uint8_t*)&currentUptime, 4);
      doRoleNegotiation = true;
    };
    void onDisconnect(BLEServer* pServer) {
      Serial.println("Server: Client disconnected");
      serverConnected = false;
      if (roleAssigned && isMaster) {
        Serial.println("Server: Master lost client, resetting roles and restarting advertising");
        roleAssigned = false;
        isMaster = false;
        isClient = false;
        randomScanDelay = random(200, 1200);
        scanDelayStart = millis();
      }
      pServer->startAdvertising();
      Serial.println("Server: Restarted advertising after client disconnect");
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
    if (roleAssigned) {
      Serial.println("Client: Resetting role assignment due to disconnection");
      roleAssigned = false;
      isMaster = false;
      isClient = false;
      randomScanDelay = random(200, 1200);
      scanDelayStart = millis();
    }
    pRemoteService = nullptr;
    pRemoteCounterCharacteristic = nullptr;
    pRemoteSyncCharacteristic = nullptr;
    pRemoteTimestampCharacteristic = nullptr;
    if (targetDevice != nullptr) {
        delete targetDevice;
        targetDevice = nullptr;
    }
    BLEDevice::startAdvertising();
    Serial.println("Client: Restarted server advertising and scanning after disconnect");
  }
};

// Sync characteristic callback
class MySyncCallback: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      struct {
        uint32_t counter;
        uint32_t timeSinceLastUpdate;
      } syncPacket;
      memcpy(&syncPacket, value.data(), 8);
      localCounter = syncPacket.counter;
      unsigned long currentTime = millis();
      unsigned long masterTimeSinceUpdate = syncPacket.timeSinceLastUpdate;
      lastCounterUpdate = currentTime - masterTimeSinceUpdate;
      Serial.printf("Timing Sync: Counter=%d, MasterTimeSinceUpdate=%lu, Current=%lu\n", 
                    syncPacket.counter, masterTimeSinceUpdate, currentTime);
      Serial.printf("Timing Sync: Set lastCounterUpdate to %lu (next increment in %lu ms)\n", 
                    lastCounterUpdate, COUNTER_INTERVAL - masterTimeSinceUpdate);
    }
};

// Advertised device scanner
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      if (advertisedDevice.haveServiceUUID() && 
          advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
        Serial.printf("Found target device: %s\n", advertisedDevice.getAddress().toString().c_str());
        if (!clientConnected || (serverConnected && !roleAssigned)) {
          BLEDevice::getScan()->stop();
          if (targetDevice != nullptr) {
            delete targetDevice;
            targetDevice = nullptr;
          }
          targetDevice = new BLEAdvertisedDevice(advertisedDevice);
          String localMac = BLEDevice::getAddress().toString().c_str();
          String remoteMac = advertisedDevice.getAddress().toString().c_str();
          if (localMac < remoteMac) {
            Serial.println("Delaying connection to avoid collision (smaller MAC)");
            delay(1000);
          }
          doConnect = true;
          doScan = false;
        } else {
          Serial.println("Already properly connected, ignoring found device");
        }
      }
    }
};

static void setupBLEServer() {
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  pService = pServer->createService(SERVICE_UUID);
  pCounterCharacteristic = pService->createCharacteristic(
    COUNTER_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pSyncCharacteristic = pService->createCharacteristic(
    SYNC_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pTimestampCharacteristic = pService->createCharacteristic(
    TIMESTAMP_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pSyncCharacteristic->setCallbacks(new MySyncCallback());
  uint32_t initialUptime = millis();
  pTimestampCharacteristic->setValue((uint8_t*)&initialUptime, 4);
  pCounterCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
  Serial.println("BLE Server started and advertising");
}

static void setupBLEClient() {
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  Serial.println("BLE Client scanner configured");
}

static void performRoleNegotiation() {
  if (!serverConnected || roleAssigned) {
    return;
  }
  Serial.println("Server: Connected without role assignment, forcing disconnection for proper negotiation");
  if (pServer != nullptr) {
    pServer->disconnect(0);
  }
  serverConnected = false;
  roleAssigned = false;
  isMaster = false;
  isClient = false;
  doScan = true;
  Serial.println("Server: Forced disconnect complete, will scan for proper reconnection");
}

static bool connectToServer() {
  Serial.printf("Attempting to connect to %s\n", targetDevice->getAddress().toString().c_str());
  if (pClient != nullptr) {
    if (pClient->isConnected()) {
      pClient->disconnect();
    }
    delete pClient;
    pClient = nullptr;
  }
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  Serial.println("Connecting to server...");
  if (!pClient->connect(targetDevice)) {
    Serial.println("Failed to connect to server - connection timeout or refused");
    delete pClient;
    pClient = nullptr;
    doConnect = false;
    doScan = true;
    return false;
  }
  Serial.println("Connected to server");
  Serial.println("Getting service...");
  pRemoteService = pClient->getService(SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find service UUID");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    doConnect = false;
    doScan = true;
    return false;
  }
  Serial.println("Found service");
  Serial.println("Getting characteristics...");
  pRemoteCounterCharacteristic = pRemoteService->getCharacteristic(COUNTER_CHARACTERISTIC_UUID);
  pRemoteSyncCharacteristic = pRemoteService->getCharacteristic(SYNC_CHARACTERISTIC_UUID);
  pRemoteTimestampCharacteristic = pRemoteService->getCharacteristic(TIMESTAMP_CHARACTERISTIC_UUID);
  if (pRemoteCounterCharacteristic == nullptr || pRemoteSyncCharacteristic == nullptr || pRemoteTimestampCharacteristic == nullptr) {
    Serial.println("Failed to find characteristics");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    doConnect = false;
    doScan = true;
    return false;
  }
  Serial.println("Found characteristics");
  Serial.println("Reading remote timestamp...");
  std::string remoteTimestampData = pRemoteTimestampCharacteristic->readValue();
  if (remoteTimestampData.length() == 4) {
    uint32_t remoteUptime;
    memcpy(&remoteUptime, remoteTimestampData.data(), 4);
    uint32_t currentUptime = millis();
    Serial.printf("Local uptime: %lu\n", currentUptime);
    Serial.printf("Remote uptime: %lu\n", remoteUptime);
    if (currentUptime > remoteUptime) {
      isMaster = true;
      isClient = false;
      Serial.println("ROLE: This device is MASTER (later boot time - been running longer)");
    } else if (currentUptime < remoteUptime) {
      isMaster = false;
      isClient = true;
      Serial.println("ROLE: This device is CLIENT (earlier boot time - just rebooted)");
    } else {
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
    }
    roleAssigned = true;
  } else {
    Serial.println("Failed to read remote timestamp");
    pClient->disconnect();
    delete pClient;
    pClient = nullptr;
    doConnect = false;
    doScan = true;
    return false;
  }
  if (isClient) {
    BLEDevice::stopAdvertising();
    clientConnected = true;
    serverConnected = false;
    Serial.println("Stopped advertising as server due to client role assignment");
    BLEDevice::getScan()->stop();
    doScan = false;
  }  else if (isMaster) {
    doScan = false;
    clientConnected = true;
    BLEDevice::getScan()->stop();
    doConnect = false;
    Serial.println("Stopped scanning and connecting as a client due to server role assignment");
    BLEDevice::startAdvertising();
  }
  return true;
}

static void performSync() {
  if (clientConnected && pRemoteSyncCharacteristic != nullptr && roleAssigned) {
    if (isMaster) {
      struct {
        uint32_t counter;
        uint32_t timeSinceLastUpdate;
      } syncPacket;
      syncPacket.counter = localCounter;
      syncPacket.timeSinceLastUpdate = millis() - lastCounterUpdate;
      pRemoteSyncCharacteristic->writeValue((uint8_t*)&syncPacket, sizeof(syncPacket));
      Serial.printf("Master: Sent timing sync - Counter: %d, TimeSinceUpdate: %lu\n", syncPacket.counter, syncPacket.timeSinceLastUpdate);
    } else if (isClient) {
      std::string value = pRemoteCounterCharacteristic->readValue();
      if (value.length() == 4) {
        memcpy(&remoteCounter, value.data(), 4);
        Serial.printf("Client sync - Master counter: %d, Local counter: %d\n", remoteCounter, localCounter);
        if (remoteCounter != localCounter) {
          localCounter = remoteCounter;
          Serial.printf("Client: Synchronized to master counter %d\n", localCounter);
          pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
          if (serverConnected) {
            pCounterCharacteristic->notify();
          }
        }
      }
    }
  }
}

static void updateCounter() {
  if (!roleAssigned) {
    localCounter++;
    Serial.printf("Standalone counter: %d\n", localCounter);
  } else {
    if (isMaster) {
      localCounter++;
      Serial.printf("Master counter: %d\n", localCounter);
    } else if (isClient) {
      localCounter++;
      if (clientConnected) {
        Serial.printf("Client counter (connected): %d\n", localCounter);
      } else {
        Serial.printf("Client counter (standalone): %d\n", localCounter);
      }
    }
  }
  pCounterCharacteristic->setValue((uint8_t*)&localCounter, 4);
  if (serverConnected) {
    pCounterCharacteristic->notify();
  }
}

void resetConnectionState() {
  Serial.println("Connection Reset: Cleaning up connection state");
  if (pClient != nullptr) {
    if (pClient->isConnected()) {
      pClient->disconnect();
    }
    delete pClient;
    pClient = nullptr;
  }
  pRemoteService = nullptr;
  pRemoteCounterCharacteristic = nullptr;
  pRemoteSyncCharacteristic = nullptr;
  pRemoteTimestampCharacteristic = nullptr;
  if (targetDevice != nullptr) {
    delete targetDevice;
    targetDevice = nullptr;
  }
  clientConnected = false;
  doConnect = false;
  if (roleAssigned) {
    Serial.println("Connection Reset: Resetting role assignment");
    roleAssigned = false;
    isMaster = false;
    isClient = false;
  }
  BLEDevice::startAdvertising();
  doScan = true;
  Serial.println("Connection state reset - ready for reconnection");
}

void BLESync_setup() {
  uint64_t chipid = ESP.getEfuseMac();
  deviceName = "ESP32Counter_" + String((uint16_t)(chipid >> 32), HEX);
  bootTimestamp = millis();
  Serial.printf("Starting %s...\n", deviceName.c_str());
  Serial.printf("Boot timestamp: %lu\n", bootTimestamp);
  BLEDevice::init(deviceName.c_str());
  setupBLEServer();
  setupBLEClient();
  doScan = true;
  lastScanAttempt = millis();
  randomSeed(esp_random());
  Serial.println("Setup complete!");
}

void BLESync_loop() {
  unsigned long currentTime = millis();
  if (doRoleNegotiation) {
    performRoleNegotiation();
    doRoleNegotiation = false;
  }
  if (currentTime - lastCounterUpdate >= COUNTER_INTERVAL) {
    updateCounter();
    lastCounterUpdate = currentTime;
  }
  if (currentTime - lastSyncTime >= SYNC_INTERVAL) {
    if (clientConnected && roleAssigned) {
      performSync();
    }
    lastSyncTime = currentTime;
  }
  if (doScan) {
    Serial.println("Starting BLE scan...");
    BLEScanResults foundDevices = BLEDevice::getScan()->start(SCAN_TIME, false);
    int deviceCount = foundDevices.getCount();
    Serial.printf("Scan complete: Found %d devices\n", deviceCount);
    for (int i = 0; i < deviceCount; i++) {
      BLEAdvertisedDevice device = foundDevices.getDevice(i);
      if (device.haveServiceUUID() && 
          device.isAdvertisingService(BLEUUID(SERVICE_UUID))){
        Serial.printf("Device %d: %s - Has our service\n", i, device.getAddress().toString().c_str());
      }
    }
    doScan = false;
    lastScanAttempt = currentTime;
  }
  if (doConnect) {
    if (connectAttemptStartTime == 0) {
      connectAttemptStartTime = currentTime;
      Serial.println("Starting connection attempt...");
    }
    if (currentTime - connectAttemptStartTime > CONNECTION_TIMEOUT) {
      Serial.println("Connection attempt timed out, resetting...");
      doConnect = false;
      connectAttemptStartTime = 0;
      doScan = true;
      if (targetDevice != nullptr) {
        delete targetDevice;
        targetDevice = nullptr;
      }
    } else {
      if (connectToServer()) {
        Serial.println("Successfully connected to server and role assigned");
        connectAttemptStartTime = 0;
      } else {
        Serial.println("Failed to connect to server or assign role");
        connectAttemptStartTime = 0;
      }
      doConnect = false;
    }
  }
  static unsigned long lastStatusPrint = 0;
  if (currentTime - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
    Serial.printf("Status - Role: %s, ClientConnToServer: %s, ServerConnToClient: %s, Counter: %d, doConnect: %s, doScan: %s\n", 
                 roleAssigned ? (isMaster ? "MASTER" : "CLIENT") : "UNASSIGNED",
                 clientConnected ? "YES" : "NO",
                 serverConnected ? "YES" : "NO",
                 localCounter,
                 doConnect ? "YES" : "NO",
                 doScan ? "YES" : "NO");
    lastStatusPrint = currentTime;
  }
  if ((!clientConnected && !serverConnected) || !roleAssigned) {
    if (!doScan && !doConnect && (currentTime - lastScanAttempt >= RESCAN_INTERVAL)) {
      Serial.println("No proper connection/role, starting periodic scan...");
      doScan = true;
    }
  }
  if (randomScanDelay > 0 && (currentTime - scanDelayStart >= randomScanDelay)) {
    doScan = true;
    randomScanDelay = 0;
    scanDelayStart = 0;
    Serial.println("Randomized delay complete, starting scan.");
  }
}

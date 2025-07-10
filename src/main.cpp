#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#include <stdlib.h> // For random

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

// Function prototypes
void resetConnectionState();

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
bool doRoleNegotiation = false;
unsigned long connectAttemptStartTime = 0;
#define CONNECTION_TIMEOUT 10000  // 10 second timeout for connection attempts
BLEAdvertisedDevice* targetDevice = nullptr;

// Add a random delay (0-1000ms) before scanning/connecting after disconnect
unsigned long randomScanDelay = 0;
unsigned long scanDelayStart = 0;

// Server callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      serverConnected = true;
      Serial.println("Server: Client connected");
      
      // Update timestamp characteristic with current uptime for role negotiation
      uint32_t currentUptime = millis();
      pTimestampCharacteristic->setValue((uint8_t*)&currentUptime, 4);
      
      // Trigger role negotiation when a client connects
      // We'll do this in the main loop to avoid blocking the callback
      doRoleNegotiation = true;
    };

    void onDisconnect(BLEServer* pServer) {
      Serial.println("Server: Client disconnected");
      serverConnected = false;
      
      // If we were in a master role and lost the client connection,
      // we need to reset and be ready to reconnect
      if (roleAssigned && isMaster) {
        Serial.println("Server: Master lost client, resetting roles and restarting advertising");
        roleAssigned = false;
        isMaster = false;
        isClient = false;
        // Instead of scanning immediately, set a random delay
        randomScanDelay = random(200, 1200); // 200-1200ms
        scanDelayStart = millis();
      }
      
      // Always restart advertising when a client disconnects
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
    
    // Reset role assignment when client connection is lost
    if (roleAssigned) {
      Serial.println("Client: Resetting role assignment due to disconnection");
      roleAssigned = false;
      isMaster = false;
      isClient = false;
      // Instead of scanning immediately, set a random delay
      randomScanDelay = random(200, 1200); // 200-1200ms
      scanDelayStart = millis();
    }
    
    // Clean up remote service pointers
    pRemoteService = nullptr;
    pRemoteCounterCharacteristic = nullptr;
    pRemoteSyncCharacteristic = nullptr;
    pRemoteTimestampCharacteristic = nullptr;

    // Clean up target device
    if (targetDevice != nullptr) {
        delete targetDevice;
        targetDevice = nullptr;
    }

    // Start advertising as a server again and begin scanning
    BLEDevice::startAdvertising();
    // doScan = true; // REMOVE this line, scanning will be triggered after delay
    Serial.println("Client: Restarted server advertising and scanning after disconnect");
  }
};

// Sync characteristic callback
class MySyncCallback: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
      std::string value = pCharacteristic->getValue();
      
      // Timing sync packet format
      struct {
        uint32_t counter;
        uint32_t timeSinceLastUpdate;
      } syncPacket;
      
      memcpy(&syncPacket, value.data(), 8);
      
      // Sync logic: use the received counter value and timing
      localCounter = syncPacket.counter;
      
      // Calculate timing adjustment using relative timing (avoids clock drift issues)
      unsigned long currentTime = millis();
      unsigned long masterTimeSinceUpdate = syncPacket.timeSinceLastUpdate;
      
      // Adjust our timing to sync with master's cycle
      // Set our lastCounterUpdate so that our next increment happens at the same relative time
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
        
        // Found a device with our service
        Serial.printf("Found target device: %s\n", advertisedDevice.getAddress().toString().c_str());
        
        // Connect if we're not already connected as a client
        // OR if we have a server connection but no role assigned (need negotiation)
        if (!clientConnected || (serverConnected && !roleAssigned)) {
          BLEDevice::getScan()->stop();
          if (targetDevice != nullptr) {
            delete targetDevice;
            targetDevice = nullptr;
          }
          targetDevice = new BLEAdvertisedDevice(advertisedDevice);
          
          // Add a small delay based on MAC address to prevent connection collisions
          // Device with "smaller" MAC address waits a bit longer to avoid both connecting simultaneously
          String localMac = BLEDevice::getAddress().toString().c_str();
          String remoteMac = advertisedDevice.getAddress().toString().c_str();
          if (localMac < remoteMac) {
            Serial.println("Delaying connection to avoid collision (smaller MAC)");
            delay(1000); // Wait 1 second if we have smaller MAC
          }
          
          doConnect = true;
          doScan = false;
        } else {
          Serial.println("Already properly connected, ignoring found device");
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
  
  // Set initial uptime in timestamp characteristic
  uint32_t initialUptime = millis();
  pTimestampCharacteristic->setValue((uint8_t*)&initialUptime, 4);
  
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
  pAdvertising->start();

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

void performRoleNegotiation() {
  // Only perform role negotiation if we have a server connection but no assigned role
  if (!serverConnected || roleAssigned) {
    return;
  }
  
  Serial.println("Server: Connected without role assignment, forcing disconnection for proper negotiation");
  
  // Force disconnection to restart the connection process with proper role negotiation
  if (pServer != nullptr) {
    pServer->disconnect(0);
  }
  
  // Reset states and start scanning
  serverConnected = false;
  roleAssigned = false;
  isMaster = false;
  isClient = false;
  doScan = true;
  
  Serial.println("Server: Forced disconnect complete, will scan for proper reconnection");
}

bool connectToServer() {
  Serial.printf("Attempting to connect to %s\n", targetDevice->getAddress().toString().c_str());
  
  // Clean up any existing client
  if (pClient != nullptr) {
    if (pClient->isConnected()) {
      pClient->disconnect();
    }
    delete pClient;
    pClient = nullptr;
  }
    pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  
  // Connect to remote BLE server with timeout handling
  Serial.println("Connecting to server...");
  if (!pClient->connect(targetDevice)) {
    Serial.println("Failed to connect to server - connection timeout or refused");
    delete pClient;
    pClient = nullptr;
    // Reset flags to allow retry
    doConnect = false;
    doScan = true;
    return false;
  }
  Serial.println("Connected to server");
    // Obtain reference to service
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
  
  // Obtain references to characteristics
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
  Serial.println("Found characteristics");    // Perform role assignment based on current uptime
  // Device with longer uptime (larger value) becomes master
  Serial.println("Reading remote timestamp...");
  std::string remoteTimestampData = pRemoteTimestampCharacteristic->readValue();
  if (remoteTimestampData.length() == 4) {
    uint32_t remoteUptime;
    memcpy(&remoteUptime, remoteTimestampData.data(), 4);
    
    // Calculate current uptime for comparison
    uint32_t currentUptime = millis();
    
    Serial.printf("Local uptime: %lu\n", currentUptime);
    Serial.printf("Remote uptime: %lu\n", remoteUptime);
      // Device with later (higher) boot timestamp becomes master (longer running time)
    if (currentUptime > remoteUptime) {
      isMaster = true;
      isClient = false;
      Serial.println("ROLE: This device is MASTER (later boot time - been running longer)");
    } else if (currentUptime < remoteUptime) {
      isMaster = false;
      isClient = true;
      Serial.println("ROLE: This device is CLIENT (earlier boot time - just rebooted)");
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
    serverConnected = false; // Reset server connection state
    Serial.println("Stopped advertising as server due to client role assignment");
    // Stop scanning as client if role assigned
    BLEDevice::getScan()->stop();
    doScan = false;
  }  else if (isMaster) {
    doScan = false; // Stop scanning if we are master
    clientConnected = true; // Keep client connection for sync operations
    // Stop all client activities if we are master
    BLEDevice::getScan()->stop();
    doConnect = false;
    Serial.println("Stopped scanning and connecting as a client due to server role assignment");
    // Ensure advertising is running as master
    BLEDevice::startAdvertising();
  }
  
  return true;
}

void performSync() {
  if (clientConnected && pRemoteSyncCharacteristic != nullptr && roleAssigned) {
    if (isMaster) {
      // Master behavior: send counter and time since last update
      struct {
        uint32_t counter;
        uint32_t timeSinceLastUpdate;
      } syncPacket;
      
      syncPacket.counter = localCounter;
      syncPacket.timeSinceLastUpdate = millis() - lastCounterUpdate; // Time elapsed since last counter increment
      
      // Write the sync packet to remote device
      pRemoteSyncCharacteristic->writeValue((uint8_t*)&syncPacket, sizeof(syncPacket));
      Serial.printf("Master: Sent timing sync - Counter: %d, TimeSinceUpdate: %lu\n", syncPacket.counter, syncPacket.timeSinceLastUpdate);
      
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
  Serial.println("Connection Reset: Cleaning up connection state");
  
  // Clean up client connection if it exists
  if (pClient != nullptr) {
    if (pClient->isConnected()) {
      pClient->disconnect();
    }
    delete pClient;
    pClient = nullptr;
  }
  
  // Reset remote service pointers
  pRemoteService = nullptr;
  pRemoteCounterCharacteristic = nullptr;
  pRemoteSyncCharacteristic = nullptr;
  pRemoteTimestampCharacteristic = nullptr;
  
  // Clean up target device
  if (targetDevice != nullptr) {
    delete targetDevice;
    targetDevice = nullptr;
  }
  
  // Reset connection flags
  clientConnected = false;
  doConnect = false;
  
  // Reset role assignment to allow renegotiation
  if (roleAssigned) {
    Serial.println("Connection Reset: Resetting role assignment");
    roleAssigned = false;
    isMaster = false;
    isClient = false;
  }
  
  // Restart advertising and scanning
  BLEDevice::startAdvertising();
  doScan = true;
  
  Serial.println("Connection state reset - ready for reconnection");
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // Keep this one delay as it's needed for serial initialization
  
  // Generate unique device name and boot timestamp
  uint64_t chipid = ESP.getEfuseMac();
  deviceName = "ESP32Counter_" + String((uint16_t)(chipid >> 32), HEX);
    // Use a combination of chip ID and current uptime for role negotiation
  // Higher uptime = device has been running longer = should be master
  bootTimestamp = millis();
  
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
  
  randomSeed(esp_random()); // Seed random for delay
  
  Serial.println("Setup complete!");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Handle role negotiation when server gets a connection without assigned roles
  if (doRoleNegotiation) {
    performRoleNegotiation();
    doRoleNegotiation = false;
  }
  
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
  }  // Handle scanning and connection
  if (doScan) {    
    Serial.println("Starting BLE scan...");
    BLEScanResults foundDevices = BLEDevice::getScan()->start(SCAN_TIME, false);
    int deviceCount = foundDevices.getCount();
    Serial.printf("Scan complete: Found %d devices\n", deviceCount);
    
    // Debug: list all found devices
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
    // Connect to discovered device
  if (doConnect) {
    if (connectAttemptStartTime == 0) {
      connectAttemptStartTime = currentTime;
      Serial.println("Starting connection attempt...");
    }
    
    // Check for connection timeout
    if (currentTime - connectAttemptStartTime > CONNECTION_TIMEOUT) {
      Serial.println("Connection attempt timed out, resetting...");
      doConnect = false;
      connectAttemptStartTime = 0;
      doScan = true;
      
      // Clean up
      if (targetDevice != nullptr) {
        delete targetDevice;
        targetDevice = nullptr;
      }
    } else {
      // Attempt connection
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
    // Print status every 10 seconds
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
  }// Periodically scan for devices if not connected or role not assigned
  if ((!clientConnected && !serverConnected) || !roleAssigned) {
    if (!doScan && !doConnect && (currentTime - lastScanAttempt >= RESCAN_INTERVAL)) {
      Serial.println("No proper connection/role, starting periodic scan...");
      doScan = true;
    }
  }
  
  // Handle random scan delay after disconnect
  if (randomScanDelay > 0 && (currentTime - scanDelayStart >= randomScanDelay)) {
    doScan = true;
    randomScanDelay = 0;
    scanDelayStart = 0;
    Serial.println("Randomized delay complete, starting scan.");
  }
  
  // Small delay to prevent overwhelming the system (non-blocking alternative)
  // Instead of delay(100), we just continue the loop - the timing checks above provide natural throttling
}
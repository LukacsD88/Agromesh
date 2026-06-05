/*
 * Project: Agro-Mesh IoT Greenhouse Monitoring
 * Role: Master Gateway (ESP32-WROOM-32U)
 * Hardware: External +3dBi Antenna recommended
 */

#include <WiFi.h>
#include <secrets.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h> // Added for Captive Portal

// ================= USER CONFIGURATION =================
// Wi-Fi credentials removed! Managed securely by WiFiManager Captive Portal.

// ThingsBoard MQTT Settings
// #define THINGSBOARD_SERVER "mqtt.thingsboard.cloud"  
// #define THINGSBOARD_TOKEN  "czki05jpydo8tt4uusx7" 
//const char* TB_SERVER = "mqtt.eu.thingsboard.cloud"; // Or your local IP
//const int   TB_PORT   = 1883;
//const char* TB_TOKEN  = "02pM2IgZnjj5XZyhQlY9"; // Token for the GATEWAY device

//local RPi4 ThingsBoard
const char* TB_SERVER = "192.168.0.119"; // Or your local IP
const int   TB_PORT   = 1883;
const char* TB_TOKEN  = "6tmwmtYXMpsPr0ZBBpqY"; // Token for the GATEWAY device

// ================= DATA STRUCTURES =================
// Must match the Slave structure EXACTLY
typedef struct __attribute__((packed)) struct_message {
  int id;             // Node ID (derived from MAC)
  uint32_t sequence_id; 
  float tempAir;
  float humAir;
  float tempSoil;
  int soilMoisture;
  float batteryVolts; // (Optional, if you calculate it on slave)
  int status_code;    
  int batteryPercent;
} struct_message;

struct_message incomingData;

// Queue to buffer data from Interrupt (ESP-NOW) to Loop (MQTT)
QueueHandle_t dataQueue;

// ================= OBJECTS =================
WiFiClient espClient;
PubSubClient client(espClient);

// ================= HELPERS =================

void setupWifi() {
  delay(10);
  Serial.println("Starting Wi-Fi Manager...");

  WiFiManager wm;
  
   //wm.resetSettings(); // Ensure this is COMMENTED OUT for normal operation!

  // --- CAPTIVE PORTAL CUSTOMIZATION ---
  // Inject custom CSS to make it look like a professional commercial product
  String customCss = "<style>"
                     "body { background-color: #f4f9f4; font-family: 'Segoe UI', Tahoma, sans-serif; color: #333; }"
                     "button, input[type='submit'], .btn { background-color: #2e7d32 !important; color: white !important; border-radius: 8px; border: none; padding: 12px 24px; font-size: 16px; font-weight: bold; transition: 0.3s; }"
                     "button:hover, input[type='submit']:hover, .btn:hover { background-color: #1b5e20 !important; cursor: pointer; transform: scale(1.02); }"
                     ".wrap { border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.1); background-color: #ffffff; padding: 30px; max-width: 400px; margin: 40px auto; border-top: 6px solid #2e7d32; }"
                     "h1 { color: #2e7d32; font-size: 26px; font-weight: 800; text-align: center; margin-bottom: 20px; }"
                     "input[type='text'], input[type='password'] { border-radius: 6px; border: 1px solid #ccc; padding: 12px; width: 100%; box-sizing: border-box; margin-bottom: 15px; font-size: 16px; }"
                     ".msg { color: #2e7d32; font-weight: bold; }"
                     "</style>";
                     
  wm.setCustomHeadElement(customCss.c_str());
  
  // Clean up the menu to only show what a commercial user needs
  std::vector<const char *> menu = {"wifi", "info", "restart"};
  wm.setMenu(menu);

  // Spins up "Agro-Mesh Setup" AP if no known networks are found
  if(!wm.autoConnect("Agro-Mesh Setup")) {
    Serial.println("Failed to connect to Wi-Fi. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("\nWi-Fi Connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
  Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());
  Serial.print("Channel: "); Serial.println(WiFi.channel());
  
  // START PAIRING BEACON (Visible):
  // The beacon must be visible (not hidden) so the Slave can read its SSID.
  // We embed the Master's true STA MAC directly into the SSID.
  String macStr = WiFi.macAddress();
  macStr.replace(":", "");
  String beaconSSID = "AgroMesh-" + macStr;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(beaconSSID.c_str(), "agromesh123", WiFi.channel(), 0); // 0 = Visible
}

void reconnectMqtt() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Connect using the Gateway Token
    if (client.connect("AgroMeshGateway", TB_TOKEN, NULL)) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// ================= CALLBACKS =================

// ESP-NOW Receive Callback (Runs in Interrupt context - keep it short!)
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingDataPtr, int len) {
  if (len != sizeof(struct_message)) {
    Serial.println("Error: Invalid packet size received");
    return;
  }
  
  // Copy data to local struct
  memcpy(&incomingData, incomingDataPtr, sizeof(incomingData));

  // Send to Queue for processing in the main loop
  // portMAX_DELAY waits indefinitely if queue is full (optional: use 0 to skip)
  xQueueSend(dataQueue, &incomingData, 0); 
}

// ================= MAIN SETUP =================

void setup() {
  Serial.begin(115200);
  
  // 1. Create Queue (Buffer size: 20 messages)
  dataQueue = xQueueCreate(20, sizeof(struct_message));
  if (dataQueue == NULL) {
    Serial.println("Error creating the queue");
  }

  // 2. Connect to Wi-Fi
  setupWifi();

  // 3. Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // 4. Register Receive Callback
  esp_now_register_recv_cb(OnDataRecv);

  // 5. Init MQTT
  client.setServer(TB_SERVER, TB_PORT);
}

// ================= MAIN LOOP =================

void loop() {
  if (!client.connected()) reconnectMqtt();
  client.loop();

  struct_message currentMsg;
  if (xQueueReceive(dataQueue, &currentMsg, 0) == pdTRUE) {
    
    // --- FLATTENED JSON STRATEGY ---
    // Instead of nesting, we create unique keys for every sensor
    // e.g. "Node_5A_Temp": 22.5
    
    StaticJsonDocument<512> doc;
    
    // Create prefix based on Node ID (e.g., "N_12AB_")
    char prefix[16];
    sprintf(prefix, "N_%X_", currentMsg.id);
    
    // Construct keys dynamically
    String keyTemp = String(prefix) + "Temp";
    String keyHum  = String(prefix) + "Hum";
    String keySoil = String(prefix) + "SoilT";
    String keyMoist= String(prefix) + "Moist";
    String keyBatV = String(prefix) + "BatV";
    String keyBatP = String(prefix) + "BatP";

    // Use serialized() to perfectly format floats to 2 decimal places in JSON
    doc[keyTemp]  = serialized(String(currentMsg.tempAir, 2));
    doc[keyHum]   = serialized(String(currentMsg.humAir, 2));
    doc[keySoil]  = serialized(String(currentMsg.tempSoil, 2));
    doc[keyMoist] = currentMsg.soilMoisture;
    doc[keyBatV]  = serialized(String(currentMsg.batteryVolts, 2));
    doc[keyBatP]  = currentMsg.batteryPercent;

    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer);

    Serial.print("Publishing Flat Data: ");
    Serial.println(jsonBuffer);

    // CRITICAL: Send to the STANDARD Device Topic, not Gateway
    client.publish("v1/devices/me/telemetry", jsonBuffer);
  }
}
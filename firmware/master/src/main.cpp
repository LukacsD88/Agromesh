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

// ================= USER CONFIGURATION =================
const char* WIFI_SSID = WIFI_SSID;
const char* WIFI_PASS = WIFI_PASSWORD;

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
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_AP_STA); // AP_STA mode is often more stable for ESP-NOW + WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi Connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
  Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());
  Serial.print("Channel: "); Serial.println(WiFi.channel());
  
  // CRITICAL: ESP-NOW must use the SAME channel as the Wi-Fi connection
  // The Slave nodes must scan to find this channel or match it.
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

    doc[keyTemp]  = currentMsg.tempAir;
    doc[keyHum]   = currentMsg.humAir;
    doc[keySoil]  = currentMsg.tempSoil;
    doc[keyMoist] = currentMsg.soilMoisture;

    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer);

    Serial.print("Publishing Flat Data: ");
    Serial.println(jsonBuffer);

    // CRITICAL: Send to the STANDARD Device Topic, not Gateway
    client.publish("v1/devices/me/telemetry", jsonBuffer);
  }
}
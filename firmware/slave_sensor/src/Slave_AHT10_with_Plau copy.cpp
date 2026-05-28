#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <esp_wifi.h> 

// ================= CONFIGURATION =================
uint8_t MASTER_MAC[] = {0x4C, 0x11, 0xAE, 0x6D, 0x2E, 0x68}; 
#define WIFI_SSID "Kiscsillag" // Your WiFi Name from the log

#define SENSOR1_SDA 8
#define SENSOR1_SCL 9
#define SENSOR2_SDA 4
#define SENSOR2_SCL 5

#define MAX_TEMP_DIFF 2.0 
#define ACK_CODE 0xAC

// ================= GLOBALS =================
typedef struct struct_message {
    uint32_t sequence_id;
    float temp;
    float hum;
    int status_code; 
} struct_message;

struct_message myData;
Adafruit_AHTX0 aht; // We use ONE object and move it around
uint32_t seq_num = 0;
volatile bool ack_received = false;

// ================= HELPER: FIND CHANNEL =================
int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks();
  for (uint8_t i=0; i<n; i++) {
    if (!strcmp(WiFi.SSID(i).c_str(), ssid)) return WiFi.channel(i);
  }
  return 0;
}

// ================= HELPER: DYNAMIC SENSOR READ =================
bool readSensorOnPins(int sda, int scl, float *t, float *h) {
  // 1. Move the Hardware I2C to these pins
  Wire.end(); // Stop previous connection
  Wire.begin(sda, scl); // Start new connection
  delay(50); // Give it a moment to settle

  // 2. Restart the AHT object
  if (!aht.begin(&Wire)) {
    return false; // Sensor not found here
  }

  // 3. Read
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  *t = temp.temperature;
  *h = humidity.relative_humidity;
  return true;
}

// ================= CALLBACKS =================
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len < 1) return;
  if (incomingData[0] == ACK_CODE) ack_received = true;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(2000);

  // --- 1. WiFi & Channel Setup ---
  WiFi.mode(WIFI_STA);
  int32_t channel = getWiFiChannel(WIFI_SSID);
  
  if (channel == 0) {
    Serial.println("WiFi not found, defaulting to Ch 1");
    channel = 1;
  } else {
    Serial.printf("Found WiFi on Ch %d\n", channel);
  }

  // Force Channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  
  // --- 2. ESP-NOW Init ---
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    ESP.restart();
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // --- 3. Add Peer (Robust) ---
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, MASTER_MAC, 6);
  peerInfo.channel = channel;  
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; // CRITICAL FIX FOR C3
  
  // Check if peer exists before adding
  if (!esp_now_is_peer_exist(MASTER_MAC)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK){
      Serial.println("Failed to add Master peer");
    } else {
      Serial.println("Master Peer Added");
    }
  }
}

// ================= LOOP =================
void loop() {
  float t1, h1, t2, h2;
  
  // --- DYNAMIC READ STRATEGY ---
  // Read Sensor 1 (Pins 8, 9)
  bool s1 = readSensorOnPins(SENSOR1_SDA, SENSOR1_SCL, &t1, &h1);
  
  // Read Sensor 2 (Pins 4, 5)
  bool s2 = readSensorOnPins(SENSOR2_SDA, SENSOR2_SCL, &t2, &h2);

  Serial.printf("Sensor 1: %s | Sensor 2: %s\n", s1?"OK":"FAIL", s2?"OK":"FAIL");

  // --- LOGIC ---
  myData.sequence_id = ++seq_num;
  
  if (s1 && s2) {
    if (abs(t1 - t2) > MAX_TEMP_DIFF) {
      myData.status_code = 3; 
    } else {
      myData.status_code = 0; 
    }
    myData.temp = (t1 + t2) / 2.0;
    myData.hum = (h1 + h2) / 2.0;
  } 
  else if (s1) { myData.status_code = 2; myData.temp = t1; myData.hum = h1; }
  else if (s2) { myData.status_code = 1; myData.temp = t2; myData.hum = h2; }
  else         { myData.status_code = 99; myData.temp = 0; myData.hum = 0; }


  //---------------resend logic----------------
bool delivery_success = false;
  
  for (int attempt = 1; attempt <= 5; attempt++) {
    ack_received = false; 
    
    // Send
    esp_err_t result = esp_now_send(MASTER_MAC, (uint8_t *)&myData, sizeof(myData));
    
    // Wait for hardware ACK
    uint32_t wait_start = millis();
    while (!ack_received && (millis() - wait_start) < 200) {
      delay(1);
    }
    
    if (ack_received) {
      Serial.printf("Sent successfully on attempt %d\n", attempt);
      delivery_success = true;
      break; // Exit the retry loop
    } else {
      Serial.printf("Attempt %d failed. Retrying...\n", attempt);
      delay(200); // Wait a tiny bit before trying again
    }
  }

  if (!delivery_success) {
    Serial.println("CRITICAL: Master is offline (5 attempts failed)");
  }

/*
  // --- SEND ---
  ack_received = false; 
  esp_err_t res = esp_now_send(MASTER_MAC, (uint8_t *)&myData, sizeof(myData));
  
  uint32_t start = millis();
  while (!ack_received && (millis() - start) < 200) delay(1);
  
  if (ack_received) Serial.println("ACK Received!");
  else Serial.println("NO ACK.");
  */

  delay(6879); 
}
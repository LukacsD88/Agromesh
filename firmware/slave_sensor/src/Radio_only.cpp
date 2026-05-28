#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// === CONFIGURATION ===
// DOUBLE CHECK THIS IS YOUR MASTER'S MAC!
uint8_t MASTER_MAC[] = {0x4C, 0x11, 0xAE, 0x6D, 0x2E, 0x68}; 
#define WIFI_SSID "Kiscsillag" // Your WiFi Name

typedef struct struct_message {
    uint32_t sequence_id;
    float temp;
    float hum;
    int status_code; 
} struct_message;

struct_message myData;
volatile bool ack_received = false;

// Scan for Channel
int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks();
  for (uint8_t i=0; i<n; i++) {
    if (!strcmp(WiFi.SSID(i).c_str(), ssid)) return WiFi.channel(i);
  }
  return 0;
}

void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len > 0 && incomingData[0] == 0xAC) ack_received = true;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- RADIO ONLY TEST ---");

  WiFi.mode(WIFI_STA);
  int32_t channel = getWiFiChannel(WIFI_SSID);
  if (channel == 0) channel = 1;

  // Force Channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("Locked to Channel %d\n", channel);

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, MASTER_MAC, 6);
  peerInfo.channel = channel;  
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) Serial.println("Peer Add Failed");
  else Serial.println("Peer Added");
}

void loop() {
  myData.sequence_id++;
  myData.temp = 25.0; // Fake data
  myData.hum = 50.0;
  myData.status_code = 0;

  ack_received = false;
  esp_now_send(MASTER_MAC, (uint8_t *)&myData, sizeof(myData));
  
  Serial.print("Ping Master... ");
  uint32_t start = millis();
  while (!ack_received && (millis() - start) < 200) delay(1);
  
  if (ack_received) Serial.println("SUCCESS! (ACK Received)");
  else Serial.println("FAILED (No ACK)");

  delay(2000);
}
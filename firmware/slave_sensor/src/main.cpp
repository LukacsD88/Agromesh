#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_random.h> // for hardware random

typedef struct {
  uint32_t rnd;
  uint32_t seq;
} payload_t;

#define ACK_CODE 0xAC

// Replace with master's MAC address (6 bytes)
uint8_t MASTER_MAC[6] = {0x4C,0x11,0xAE,0x6D,0x2E,0x68}; // <-- CHANGE to master MAC 4C:11:AE:6D:2E:68


static uint32_t seq_num = 0;
volatile bool ack_received = false;
volatile uint8_t last_ack = 0;

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // send callback: status indicates whether packet was transmitted successfully
  // we don't use it here for ACK detection; ack will arrive via recv callback
  (void) mac_addr;
  (void) status;
}

void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len < 1) return;
  last_ack = incomingData[0];
  if (last_ack == ACK_CODE) {
    ack_received = true;
  }
}

void initPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, MASTER_MAC, 6);
  peerInfo.channel = 0; // use current channel
  peerInfo.encrypt = false;
  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res != ESP_OK && res != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("Add peer failed: %d\n", res);
  } else {
    Serial.println("Peer added");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Slave booting...");

  WiFi.mode(WIFI_STA);
  // Print own MAC for convenience
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("Slave MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  initPeer();
}

void loop() {
  // generate random number
  payload_t p;
  p.rnd = esp_random(); // hardware RNG
  p.seq = ++seq_num;

  ack_received = false;

  esp_err_t res = esp_now_send(MASTER_MAC, (uint8_t *)&p, sizeof(p));
  if (res == ESP_OK) {
    Serial.printf("Sent rnd=%u seq=%u\n", p.rnd, p.seq);
  } else {
    Serial.printf("Send failed: %d\n", res);
  }

  // wait for ACK up to 2 seconds
  uint32_t start = millis();
  while (!ack_received && (millis() - start) < 2000) {
    delay(10);
  }
  if (ack_received) {
    Serial.println("ACK received from master");
  } else {
    Serial.println("No ACK received");
  }

  delay(5000); // send every 5 seconds
}

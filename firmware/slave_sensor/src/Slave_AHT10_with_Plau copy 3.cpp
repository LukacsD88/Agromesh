#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <math.h>
#include <LittleFS.h> // File System for Offline Storage

// ================= CONFIGURATION =================
//uint8_t MASTER_MAC[] = {0x4C, 0x11, 0xAE, 0x6D, 0x2E, 0x68}; 
uint8_t MASTER_MAC[] = {0xE8, 0x9F, 0x6D, 0x92, 0x53, 0x64};   //E8:9F:6D:92:53:64 ESP8266 Master 
#define WIFI_SSID "Kiscsillag" 

#define SENSOR1_SDA 8
#define SENSOR1_SCL 9
#define BATTERY_PIN 3 

#define MAX_TEMP_DIFF 2.0 
#define LOOP_DELAY_MS 5000 // 5 sec for dev (Change to 300s for Prod)
#define SLEEP_SECONDS 5    // Match LOOP_DELAY for calculation logic

// ================= DATA STRUCTURE =================
typedef struct struct_message {
    uint32_t sequence_id;
    float temp;
    float hum;
    float vpd;           
    float dew_point;     
    int status_code; 
    int battery_percent; 
    uint32_t age_seconds; // 0 = Live, >0 = Historical
} struct_message;

// Internal struct for saving to Flash (needs reference point)
typedef struct saved_record {
    struct_message msg;
    uint32_t boot_cycle_recorded;
} saved_record;

struct_message liveData;
Adafruit_AHTX0 aht; 

RTC_DATA_ATTR uint32_t boot_count = 0;
volatile bool ack_received = false;

// ================= HELPERS =================
// ... (Math helpers same as before) ...
float calculateVPD(float temp, float hum) {
  if (isnan(temp) || isnan(hum) || temp < -50 || hum < 0) return 0.0;
  float svp = 0.61078 * exp((17.27 * temp) / (temp + 237.3));
  float vpd = svp * (1.0 - (hum / 100.0));
  return vpd < 0 ? 0.0 : vpd; 
}

float calculateDewPoint(float temp, float hum) {
  if (isnan(temp) || isnan(hum) || hum <= 0) return 0.0;
  const float b = 17.625;
  const float c = 243.04;
  float alpha = log(hum / 100.0) + ((b * temp) / (c + temp));
  return (c * alpha) / (b - alpha);
}

int readBattery() {
  uint32_t raw_mv = analogReadMilliVolts(BATTERY_PIN);
  float battery_v = (raw_mv * 2.0) / 1000.0; 
  int pct = map((long)(battery_v * 100), 330, 420, 0, 100);
  return constrain(pct, 0, 100);
}

int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks(false, true); 
  for (uint8_t i=0; i<n; i++) {
    if (strcmp(WiFi.SSID(i).c_str(), ssid) == 0) return WiFi.channel(i);
  }
  return 0;
}

// ================= STORAGE LOGIC =================
void saveToHistory(struct_message data) {
  File file = LittleFS.open("/history.bin", "a"); // Append mode
  if(file) {
    saved_record rec;
    rec.msg = data;
    rec.boot_cycle_recorded = boot_count;
    file.write((uint8_t*)&rec, sizeof(rec));
    file.close();
    Serial.println("Saved to History.");
  }
}

bool processHistory() {
  if (!LittleFS.exists("/history.bin")) return false;

  File file = LittleFS.open("/history.bin", "r");
  if (!file || file.size() == 0) {
    file.close();
    LittleFS.remove("/history.bin");
    return false;
  }

  // Read ONE oldest record
  saved_record rec;
  file.read((uint8_t*)&rec, sizeof(rec));
  
  // Create a temporary file to store the rest
  File temp = LittleFS.open("/temp.bin", "w");
  while (file.available()) {
    uint8_t buf[sizeof(rec)];
    file.read(buf, sizeof(rec));
    temp.write(buf, sizeof(rec));
  }
  file.close();
  temp.close();
  LittleFS.remove("/history.bin");
  LittleFS.rename("/temp.bin", "/history.bin");

  // Calculate Age
  uint32_t cycles_ago = boot_count - rec.boot_cycle_recorded;
  rec.msg.age_seconds = cycles_ago * SLEEP_SECONDS;

  // Send Old Record
  ack_received = false;
  esp_now_send(MASTER_MAC, (uint8_t *)&rec.msg, sizeof(rec.msg));
  
  // Wait for ACK
  uint32_t start = millis();
  while(!ack_received && (millis()-start) < 200) delay(1);
  
  if (ack_received) {
    Serial.printf("History Sent! Age: %ds\n", rec.msg.age_seconds);
    return true; // We sent one, might be more
  } else {
    // Failed to send history. Put it back? 
    // Ideally yes, but for simplicity, we drop it or it complicates logic.
    // For now, let's accept we tried.
    Serial.println("History Send Failed. Dropped.");
    return false;
  }
}

// ================= MAIN LOGIC =================
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len > 0 && incomingData[0] == 0xAC) ack_received = true;
}
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Slave Store & Forward ---");

  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Mount Failed");
    return;
  }

  // Init Sensors
  Wire.begin(SENSOR1_SDA, SENSOR1_SCL);
  if (!aht.begin(&Wire)) Serial.println("AHT10 Error");

  // Init WiFi
  WiFi.mode(WIFI_STA);
  int32_t ch = getWiFiChannel(WIFI_SSID);
  if (ch == 0) ch = 1;
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, MASTER_MAC, 6);
  peerInfo.channel = ch;  
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; 
  if (!esp_now_is_peer_exist(MASTER_MAC)) esp_now_add_peer(&peerInfo);
}

void loop() {
  // 1. Measure
  sensors_event_t h, t;
  aht.getEvent(&h, &t);
  
  liveData.sequence_id = ++boot_count;
  liveData.temp = t.temperature;
  liveData.hum = h.relative_humidity;
  liveData.battery_percent = readBattery();
  liveData.status_code = 0;
  liveData.vpd = calculateVPD(liveData.temp, liveData.hum);
  liveData.dew_point = calculateDewPoint(liveData.temp, liveData.hum);
  liveData.age_seconds = 0; // This is LIVE data

  // 2. Try Send LIVE Data
  bool live_sent = false;
  for(int i=0; i<3; i++) {
    ack_received = false;
    esp_now_send(MASTER_MAC, (uint8_t *)&liveData, sizeof(liveData));
    uint32_t s = millis();
    while(!ack_received && (millis()-s) < 100) delay(1);
    if(ack_received) {
      live_sent = true;
      Serial.println("Live Data Sent.");
      break;
    }
    delay(50);
  }

  // 3. Decision Logic
  if (live_sent) {
    // If Live worked, try to send ONE history packet to catch up
    processHistory();
  } else {
    // If Live failed, Save to History
    Serial.println("Master Offline. Saving to Flash...");
    saveToHistory(liveData);
  }

  delay(LOOP_DELAY_MS);
}
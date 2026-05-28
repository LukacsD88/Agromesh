#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <math.h>

// ================= CONFIGURATION =================
// uint8_t MASTER_MAC[] = {0x4C, 0x11, 0xAE, 0x6D, 0x2E, 0x68}; 
uint8_t MASTER_MAC[] = {0xE8, 0x9F, 0x6D, 0x92, 0x53, 0x64};  
#define WIFI_SSID "Kiscsillag" 

// ESP32-C3 Super Mini Pin Mapping
#define SENSOR1_SDA 8
#define SENSOR1_SCL 9
// #define SENSOR2_SDA 4 // Disabled
// #define SENSOR2_SCL 5 // Disabled

#define BATTERY_PIN 3 // ADC1_CH3
#define SOIL_PIN    1 // ADC1_CH1 (Analog Read)
#define SOIL_PWR    0 // GPIO 0 (Power Switch for Sensor)

// Soil Calibration (Inverse logic: High Value = Dry, Low Value = Wet)
// You must calibrate these by testing! 
// Dry: Sensor in air. Wet: Sensor in cup of water.
#define SOIL_DRY 3500 
#define SOIL_WET 1400 

#define MAX_TEMP_DIFF 2.0 
#define LOOP_DELAY_MS 5000 

// ================= DATA STRUCTURE =================
// WARNING: You MUST update the Master struct to match this!
typedef struct struct_message {
    uint32_t sequence_id;
    float temp;
    float hum;
    float vpd;           
    float dew_point;     
    int status_code; 
    int battery_percent;
    int soil_percent;    // [NEW] Soil Moisture
} struct_message;

struct_message myData;
Adafruit_AHTX0 aht; 

uint32_t boot_count = 0;
volatile bool ack_received = false;

// ================= MATH HELPERS =================

float calculateVPD(float temp, float hum) {
  if (isnan(temp) || isnan(hum) || temp < -50 || hum < 0) return 0.0;
  float svp = 0.61078 * exp((17.27 * temp) / (temp + 237.3));
  float vpd = svp * (1.0 - (hum / 100.0));
  if (vpd < 0) vpd = 0.0;
  return vpd; 
}

float calculateDewPoint(float temp, float hum) {
  if (isnan(temp) || isnan(hum) || hum <= 0) return 0.0;
  const float b = 17.625;
  const float c = 243.04;
  float alpha = log(hum / 100.0) + ((b * temp) / (c + temp));
  float dew = (c * alpha) / (b - alpha);
  return dew;
}

int readBattery() {
  uint32_t raw_mv = analogReadMilliVolts(BATTERY_PIN);
  float battery_v = (raw_mv * 2.0) / 1000.0; 
  int pct = map((long)(battery_v * 100), 330, 420, 0, 100);
  if(pct > 100) pct = 100;
  if(pct < 0) pct = 0;
  return pct;
}

int readSoil() {
  // 1. Power ON the sensor
  digitalWrite(SOIL_PWR, HIGH);
  delay(50); // Wait for electronics to stabilize
  
  // 2. Read
  int raw = analogRead(SOIL_PIN);
  
  // 3. Power OFF (Prevents corrosion)
  digitalWrite(SOIL_PWR, LOW);
  
  // 4. Convert to %
  // Map raw values to 0-100%. Note: DRY is usually HIGH value.
  int pct = map(raw, SOIL_DRY, SOIL_WET, 0, 100);
  
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  
  // Debug print to help you calibrate
  Serial.printf("[Soil] Raw: %d -> %d%%\n", raw, pct);
  
  return pct;
}

int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks(false, true); 
  for (uint8_t i=0; i<n; i++) {
    if (strcmp(WiFi.SSID(i).c_str(), ssid) == 0) return WiFi.channel(i);
  }
  return 0;
}

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if (len > 0 && incomingData[0] == 0xAC) ack_received = true;
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n--- Slave Starting (Soil Enabled) ---");

  // Init Soil Pins
  pinMode(SOIL_PWR, OUTPUT);
  digitalWrite(SOIL_PWR, LOW); // Start OFF
  pinMode(SOIL_PIN, INPUT);

  // 1. Init I2C
  Wire.begin(SENSOR1_SDA, SENSOR1_SCL);
  if (!aht.begin(&Wire)) {
    Serial.println("AHT10 Not Found!");
  }

  // 2. WiFi Init
  WiFi.mode(WIFI_STA);
  int32_t channel = getWiFiChannel(WIFI_SSID);
  if (channel == 0) channel = 1;
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.printf("Locked to Channel %d\n", channel);

  // 3. ESP-NOW Init
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Failed");
    ESP.restart();
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // 4. Add Peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, MASTER_MAC, 6);
  peerInfo.channel = channel;  
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; 
  
  if (!esp_now_is_peer_exist(MASTER_MAC)) {
    esp_now_add_peer(&peerInfo);
  }
}

// ================= LOOP =================
void loop() {
  // 1. Read Sensors
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  
  int bat = readBattery();
  int soil = readSoil(); // Read soil moisture

  // 2. Populate Data
  myData.sequence_id = ++boot_count;
  myData.battery_percent = bat;
  myData.soil_percent = soil;
  
  myData.temp = temp.temperature;
  myData.hum = humidity.relative_humidity;
  myData.status_code = 0; 

  myData.vpd = calculateVPD(myData.temp, myData.hum);
  myData.dew_point = calculateDewPoint(myData.temp, myData.hum);

  // 3. Send w/ Retry
  bool sent = false;
  for(int i=0; i<5; i++) {
    ack_received = false;
    esp_now_send(MASTER_MAC, (uint8_t *)&myData, sizeof(myData));
    
    uint32_t start = millis();
    while(!ack_received && (millis()-start) < 200) delay(1);
    
    if(ack_received) {
      sent = true;
      Serial.printf("Sent SEQ:%d | Temp: %.2f | Soil: %d%% | Bat: %d%%\n", myData.sequence_id, myData.temp, soil, bat);
      break;
    }
    delay(100);
  }

  if (!sent) Serial.println("No ACK from Master.");

  delay(LOOP_DELAY_MS);
}
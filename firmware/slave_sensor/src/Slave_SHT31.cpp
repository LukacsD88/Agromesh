/*
 * Project: Agro-Mesh IoT Greenhouse Monitoring
 * Role: Slave Node (ESP32-C3 Super Mini)
 * Fix: I2C moved to 6/7 to avoid LED conflict on Pin 8
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h> 

// ================= USER CONFIGURATION =================

// UNCOMMENT FOR DEBUGGING (NO SLEEP)
// #define DEBUG_MODE  

uint8_t masterMac[] = {0xD4, 0xE9, 0xF4, 0xA4, 0xD8, 0x50}; 
#define WIFI_SSID "Kiscsillag" 

#ifdef DEBUG_MODE
  #define CYCLE_DELAY_MS 5000  
#else
  #define SLEEP_SECONDS 20     
#endif

// --- PINS (UPDATED) ---
#define PIN_SDA       6  // Moved from 8 to 6
#define PIN_SCL       7  // Moved from 9 to 7
#define PIN_DS18B20   10        
#define PIN_SOIL_ADC  3
#define PIN_LED       8  // Safe to use now!

#define SHT31_ADDR    0x44 //Sample comment: 0x44 or 0x45 depending on ADDR pin
#define HUMIDITY_SATURATION_THRESHOLD 95.0 

// ================= OBJECTS =================
Adafruit_SHT31 sht31 = Adafruit_SHT31();
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Adafruit_NeoPixel pixels(1, PIN_LED, NEO_GRB + NEO_KHZ800);

// ================= DATA STRUCTURES =================
typedef struct __attribute__((packed)) struct_message {
  int id;             
  uint32_t sequence_id; 
  float tempAir;      
  float humAir;       
  float tempSoil;     
  int soilMoisture;   
  float batteryVolts; 
  int status_code;    
} struct_message;

struct_message myData;

RTC_DATA_ATTR int savedChannel = 1; 
RTC_DATA_ATTR uint32_t boot_count = 0;

enum State {
  STATE_INIT,
  STATE_READ_SENSORS,
  STATE_TRANSMIT,
  STATE_CHANNEL_SCAN,
  STATE_SLEEP,
  STATE_ERROR
};

State currentState = STATE_INIT;
esp_now_peer_info_t peerInfo;
volatile bool deliverySuccess = false;

// ================= HELPERS =================
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) deliverySuccess = true;
}

void setupEspNow(int channel) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_is_peer_exist(masterMac)) {
      esp_now_del_peer(masterMac);
  }
  
  peerInfo.channel = channel;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; 
  memcpy(peerInfo.peer_addr, masterMac, 6);
  esp_now_add_peer(&peerInfo);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(100); 
  
  // LED Init
  pixels.begin();
  pixels.setBrightness(30); // Visible but saves power
  setLed(0, 0, 50); // Blue: Booting
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_send_cb(OnDataSent);

  currentState = STATE_INIT;
  
  #ifdef DEBUG_MODE
    Serial.println("DEBUG MODE");
  #endif
}

// ================= MAIN LOOP =================
void loop() {
  
  switch (currentState) {
    
    // --- 1. INITIALIZATION ---
    case STATE_INIT:
      // Init I2C on NEW PINS
      Wire.begin(PIN_SDA, PIN_SCL); 
      
      if (!sht31.begin(SHT31_ADDR)) {
        // Try alternate address
        if(!sht31.begin(0x45)) Serial.println("SHT31 Init Fail");
      }
      sensors.begin();
      
      currentState = STATE_READ_SENSORS;
      break;

    // --- 2. SENSOR READING ---
    case STATE_READ_SENSORS:
    {
      setLed(50, 30, 0); // Orange: Reading
      
      bool airSensorOk = true;
      bool soilSensorOk = true;

      // SHT31
      sht31.heater(false); 
      myData.tempAir = sht31.readTemperature();
      myData.humAir = sht31.readHumidity();
      
      if (isnan(myData.tempAir) || isnan(myData.humAir)) {
        myData.tempAir = 0.0; myData.humAir = 0.0;
        airSensorOk = false;
      }

      // DS18B20
      sensors.requestTemperatures(); 
      myData.tempSoil = sensors.getTempCByIndex(0);
      if (myData.tempSoil == -127.00 || myData.tempSoil == 85.00) { 
        myData.tempSoil = 0.0; soilSensorOk = false;
      }

      // ADC
      myData.soilMoisture = analogRead(PIN_SOIL_ADC);
      
      // Status Code
      if (airSensorOk && soilSensorOk) myData.status_code = 0;
      else if (!airSensorOk && soilSensorOk) myData.status_code = 1;
      else if (airSensorOk && !soilSensorOk) myData.status_code = 2;
      else myData.status_code = 99;

      // ID & Boot Count
      uint64_t chipid = ESP.getEfuseMac();
      myData.id = (uint16_t)(chipid >> 32);
      myData.batteryVolts = 0.0; 
      
      #ifdef DEBUG_MODE
        boot_count++; 
      #else
        boot_count++; // RTC var
      #endif
      myData.sequence_id = boot_count;
      
      Serial.printf("SEQ:%d Stat:%d\n", boot_count, myData.status_code);

      currentState = STATE_TRANSMIT;
      break;
    }

    // --- 3. TRANSMISSION ---
    case STATE_TRANSMIT:
      setupEspNow(savedChannel);
      deliverySuccess = false;
      esp_now_send(masterMac, (uint8_t *) &myData, sizeof(myData));
      
      {
        uint32_t start = millis();
        // Wait max 100ms for ACK
        while (!deliverySuccess && (millis() - start) < 100) delay(1);
      }

      if (deliverySuccess) {
        setLed(0, 50, 0); // Green: Success
        delay(50);        
        currentState = STATE_SLEEP;
      } else {
        setLed(50, 0, 50); // Purple: Scan needed
        currentState = STATE_CHANNEL_SCAN;
      }
      break;

    // --- 4. CHANNEL SCAN ---
    case STATE_CHANNEL_SCAN:
      {
        bool found = false;
        for (int ch = 1; ch <= 13; ch++) {
          if (ch == savedChannel) continue; 
          
          setupEspNow(ch);
          deliverySuccess = false;
          esp_now_send(masterMac, (uint8_t *) &myData, sizeof(myData));
          
          uint32_t start = millis();
          while (!deliverySuccess && (millis() - start) < 50) delay(1);
          
          if (deliverySuccess) {
            savedChannel = ch; 
            found = true;
            break;
          }
        }
        
        if (found) {
          setLed(0, 50, 0); // Green
          delay(50);
          currentState = STATE_SLEEP;
        } else {
          currentState = STATE_ERROR;
        }
      }
      break;

    // --- 5. ERROR ---
    case STATE_ERROR:
      setLed(50, 0, 0); // Red: Failed
      delay(200); 
      currentState = STATE_SLEEP;
      break;

    // --- 6. SLEEP ---
    case STATE_SLEEP:
      setLed(0, 0, 0); // OFF (Important!)

      #ifdef DEBUG_MODE
        delay(CYCLE_DELAY_MS);
        currentState = STATE_READ_SENSORS;
      #else
        Serial.flush(); 
        esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
        esp_deep_sleep_start();
      #endif
      break;
  }
}
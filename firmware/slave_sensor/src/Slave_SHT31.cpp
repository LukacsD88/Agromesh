/*
 * Project: Agro-Mesh IoT Greenhouse Monitoring
 * Role: Slave Node (ESP32-C3 Super Mini)
 * Fix: I2C moved to 6/7 to avoid LED conflict on Pin 8
 */

#include <Arduino.h>
#include <WiFi.h>
#include <secrets.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h> 
#include <Preferences.h> // Added for NVS (Non-Volatile Storage)

// ================= USER CONFIGURATION =================

// UNCOMMENT FOR DEBUGGING (NO SLEEP)
// #define DEBUG_MODE  

// Master MAC is now dynamically paired and stored!
uint8_t masterMac[6] = {0}; 
Preferences preferences;

#define WIFI_SSID WIFI_SSID
#define WIFI_PASSWORD WIFI_PASSWORD

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
#define PIN_BATTERY_ADC 1 // Added for Battery Voltage reading
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
  int batteryPercent; 
} struct_message;

struct_message myData;

RTC_DATA_ATTR int savedChannel = 1; 
RTC_DATA_ATTR uint32_t boot_count = 0;

enum State {
  STATE_PAIRING, // New state for Proximity Auto-Discovery
  STATE_INIT,
  STATE_READ_SENSORS,
  STATE_TRANSMIT,
  STATE_CHANNEL_SCAN,
  STATE_SLEEP,
  STATE_ERROR
};

State currentState;
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

// ================= BATTERY READING =================
float readBatteryVoltage() {
  // Force ADC to use full range (11dB = up to ~2.5V - 3.1V range)
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  uint32_t totalMv = 0;
  uint32_t highest = 0;
  uint32_t lowest = 9999;
  const int numSamples = 256; 

  // Throwaway the first 5 reads to let the internal ADC stabilize
  for(int i=0; i<5; i++) {
    analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }

  // Take 256 quick readings to crush the noise
  for (int i = 0; i < numSamples; i++) {
      uint32_t mv = analogReadMilliVolts(PIN_BATTERY_ADC);
      if (mv < lowest) lowest = mv;
      if (mv > highest) highest = mv;
      totalMv += mv;
      delay(1); 
  }

  // Remove the single highest and lowest noise spikes, then average
  totalMv = totalMv - highest - lowest;
  float avgMv = (float)totalMv / (numSamples - 2);
  float pinVoltage = avgMv / 1000.0;

  // Reverting to the proven 9.293 multiplier that provided the most stable values
  const float CALIBRATION_MULTIPLIER = 9.293; 
  float finalVoltage = pinVoltage * CALIBRATION_MULTIPLIER;
  
  // Round to 2 decimal places to hide the remaining microscopic hardware jitter
  return round(finalVoltage * 100.0) / 100.0;
}

// Convert voltage to a 0-100% scale (LiPo standard 3.3V - 4.2V)
int getBatteryPercentage(float voltage) {
  if (voltage >= 4.20) return 100;
  if (voltage <= 3.30) return 0;
  return (int)((voltage - 3.30) / (4.20 - 3.30) * 100.0);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(100); 
  
  // Read battery FIRST, before turning on the LED or WiFi to avoid noise
  myData.batteryVolts = readBatteryVoltage();
  myData.batteryPercent = getBatteryPercentage(myData.batteryVolts);
  
  // LED Init
  pixels.begin();
  pixels.setBrightness(30); // Visible but saves power
  setLed(0, 0, 50); // Blue: Booting
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  // NVS MAC Load: Check if we have already paired to a Master in a previous life
  preferences.begin("agromesh", false);
  if (preferences.getBytesLength("masterMac") == 6) {
    preferences.getBytes("masterMac", masterMac, 6);
    currentState = STATE_INIT; // Skip pairing, go straight to work
  } else {
    currentState = STATE_PAIRING; // Factory fresh, wait for physical proximity
  }

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_send_cb(OnDataSent);
  
  #ifdef DEBUG_MODE
    Serial.println("DEBUG MODE");
  #endif
}

// ================= MAIN LOOP =================
void loop() {
  
  switch (currentState) {

    // --- 0. PROXIMITY PAIRING (Gatekeeper Logic) ---
    case STATE_PAIRING:
    {
      setLed(0, 0, 50); // Pulsing Blue: Waiting for proximity pair
      
      int n = WiFi.scanNetworks(false, true); // true = show hidden networks
      bool paired = false;
      
      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.startsWith("AgroMesh-")) {
          int rssi = WiFi.RSSI(i);
          Serial.printf("Master Found! RSSI: %d\n", rssi);
          
          // Proximity Gatekeeper: Signal must be intensely strong (> -45 dBm = physical touch)
          if (rssi >= -45) {
            // Extract the true STA MAC from the SSID (e.g. "AgroMesh-4C11AE6D2E68")
            String macHex = ssid.substring(9);
            for(int j = 0; j < 6; j++) {
              masterMac[j] = (uint8_t) strtol(macHex.substring(j*2, j*2+2).c_str(), NULL, 16);
            }
            
            preferences.putBytes("masterMac", masterMac, 6); // Save permanently to NVS
            savedChannel = WiFi.channel(i);                // Save current channel
            
            paired = true;
            setLed(0, 50, 0); // Solid Green: Successfully Paired!
            delay(2000);
            currentState = STATE_INIT;
            break;
          } else {
            Serial.println("Signal too weak! Move Slave closer to Master.");
            setLed(50, 0, 0); // Quick Red flash: Denied
            delay(1000);
          }
        }
      }
      
      WiFi.scanDelete();
      if (!paired) delay(1000); // Loop again until physically touched
      break;
    }
    
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
        
        // Cleanly shut down the radio to prevent crashes during sleep
        esp_now_deinit();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        
        esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
        esp_deep_sleep_start();
      #endif
      break;
  }
}
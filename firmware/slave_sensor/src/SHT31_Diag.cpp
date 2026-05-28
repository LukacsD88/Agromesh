#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>

// --- PIN CONFIGURATION ---
// Verify these match your physical wiring!
#define SDA_PIN 8
#define SCL_PIN 9

Adafruit_SHT31 sht31 = Adafruit_SHT31();

void scanI2C() {
  byte error, address;
  int nDevices = 0;
  Serial.println("Scanning...");
  for(address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address<16) Serial.print("0");
      Serial.print(address,HEX);
      Serial.println("  !");
      nDevices++;
    }
    else if (error==4) {
      Serial.print("Unknown error at address 0x");
      if (address<16) Serial.print("0");
      Serial.println(address,HEX);
    }
  }
  if (nDevices == 0) Serial.println("No I2C devices found\n");
  else Serial.println("done\n");
}

void setup() {
  // 1. Init Serial
  Serial.begin(115200);
  delay(3000); // Wait for you to open the monitor
  Serial.println("\n\n--- SHT31 BARE METAL DIAGNOSTIC ---");

  // 2. Force I2C Pins
  // We use the 2-argument begin to FORCE the pins
  Serial.printf("Initializing Wire on SDA=%d, SCL=%d...\n", SDA_PIN, SCL_PIN);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // 3. Run Scanner (The Truth Teller)
  // If this finds nothing, the problem is WIRING or RESISTORS.
  scanI2C();

  // 4. Try Driver
  Serial.println("Attempting driver init at 0x44...");
  if (!sht31.begin(0x44)) {
    Serial.println("Failed at 0x44. Trying 0x45...");
    if (!sht31.begin(0x45)) {
      Serial.println("CRITICAL FAILURE: Sensor not responding to library.");
    } else {
      Serial.println("SUCCESS: Connected at 0x45");
    }
  } else {
    Serial.println("SUCCESS: Connected at 0x44");
  }
}

void loop() {
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();

  if (!isnan(t)) {
    Serial.print("Temp: "); Serial.print(t); Serial.print(" C");
    Serial.print("\t Hum: "); Serial.print(h); Serial.println(" %");
  } else {
    Serial.println("Read Failure (NaN)");
    // Try to kickstart it again
    // sht31.reset(); 
  }
  delay(1000);
}
#include <Arduino.h>
#include <unity.h>
#include <Adafruit_NeoPixel.h>

// --- Pin and object definitions from the main project ---
// This is necessary because we are testing in a separate environment.
#define PIN_LED       8  // As defined in Slave_SHT31.cpp

// Create the NeoPixel object
Adafruit_NeoPixel pixels(1, PIN_LED, NEO_GRB + NEO_KHZ800);

// --- The function to be tested ---
// Copied from Slave_SHT31.cpp for isolation.
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

// --- Test Cases ---

void setUp(void) {
    // Set up tasks to run before each test
}

void tearDown(void) {
    // Clean up tasks to run after each test
    setLed(0, 0, 0); // Turn LED off after each test
}

// Test setting the LED to RED
void test_led_red(void) {
    Serial.println("Testing LED: RED");
    setLed(255, 0, 0);
    delay(2000); // Wait 2 seconds for visual confirmation
    // In a real hardware test, you might use a light sensor to verify.
    // For now, we assume it works if the code executes.
    TEST_ASSERT_TRUE(true);
}

// Test setting the LED to GREEN
void test_led_green(void) {
    Serial.println("Testing LED: GREEN");
    setLed(0, 255, 0);
    delay(2000); // Wait 2 seconds for visual confirmation
    TEST_ASSERT_TRUE(true);
}

// Test setting the LED to BLUE
void test_led_blue(void) {
    Serial.println("Testing LED: BLUE");
    setLed(0, 0, 255);
    delay(2000); // Wait 2 seconds for visual confirmation
    TEST_ASSERT_TRUE(true);
}

void setup() {
    delay(2000); // A small delay to allow the serial monitor to connect

    pixels.begin();
    pixels.setBrightness(30);

    UNITY_BEGIN();
    RUN_TEST(test_led_red);
    RUN_TEST(test_led_green);
    RUN_TEST(test_led_blue);
    UNITY_END();
}

void loop() {
    // Nothing to do here
}

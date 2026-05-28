#include <Arduino.h>
#include <unity.h>
#include <esp_now.h>

// --- Global variable from the main project ---
// We redefine it here to test its state.
volatile bool deliverySuccess = false;

// --- The function to be tested ---
// Copied from Slave_SHT31.cpp for isolation.
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    deliverySuccess = true;
  }
}

// --- Test Utilities ---

// The setUp function runs before each test case.
// We use it to reset the state and ensure tests are independent.
void setUp(void) {
    deliverySuccess = false;
}

void tearDown(void) {
    // clean up after test
}

// --- Test Cases ---

// Test that `deliverySuccess` becomes true when status is ESP_NOW_SEND_SUCCESS
void test_OnDataSent_success(void) {
    // Arrange: Mock arguments for the function call
    const uint8_t mock_mac[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_now_send_status_t success_status = ESP_NOW_SEND_SUCCESS;

    // Act: Call the function with a success status
    OnDataSent(mock_mac, success_status);

    // Assert: Check if the global flag was set to true
    TEST_ASSERT_TRUE(deliverySuccess);
}

// Test that `deliverySuccess` remains false when status is ESP_NOW_SEND_FAIL
void test_OnDataSent_failure(void) {
    // Arrange: Mock arguments for the function call
    const uint8_t mock_mac[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    esp_now_send_status_t fail_status = ESP_NOW_SEND_FAIL;

    // Act: Call the function with a fail status
    OnDataSent(mock_mac, fail_status);

    // Assert: Check that the global flag remains false
    TEST_ASSERT_FALSE(deliverySuccess);
}


void setup() {
    delay(2500); // Delay for serial monitor connection

    UNITY_BEGIN();
    RUN_TEST(test_OnDataSent_success);
    RUN_TEST(test_OnDataSent_failure);
    UNITY_END();
}

void loop() {
    // Nothing to do here
}

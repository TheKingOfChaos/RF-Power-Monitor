// On-device tests for the W5500 Ethernet hardware and DHCP.
// Needs the W5500 wired as in pins.h and an Ethernet cable to a network with a DHCP server.
// Run: pio test -e pico -f embedded/test_ethernet

#include <Arduino.h>
#include <hardware/watchdog.h>
#include <unity.h>
#include <W5500lwIP.h>
#include "pins.h"

static Wiznet5500lwIP eth(W5500_CS_PIN, SPI, W5500_INT_PIN);
static bool ethStarted = false;

const uint32_t LINK_TIMEOUT_MS = 5000;
const uint32_t DHCP_TIMEOUT_MS = 20000;

void setUp() {}
void tearDown() {}

static void test_w5500_detected() {
    pinMode(W5500_RST_PIN, OUTPUT);
    digitalWrite(W5500_RST_PIN, LOW);
    delay(1);
    digitalWrite(W5500_RST_PIN, HIGH);
    delay(50);

    SPI.setRX(W5500_MISO_PIN);
    SPI.setCS(W5500_CS_PIN);
    SPI.setSCK(W5500_SCK_PIN);
    SPI.setTX(W5500_MOSI_PIN);

    ethStarted = eth.begin();
    TEST_ASSERT_TRUE_MESSAGE(ethStarted, "W5500 not detected, check SPI wiring");
}

static void test_link_comes_up() {
    TEST_ASSERT_TRUE_MESSAGE(ethStarted, "skipped: W5500 not started");
    uint32_t start = millis();
    while (!eth.isLinked() && millis() - start < LINK_TIMEOUT_MS) delay(50);
    TEST_ASSERT_TRUE_MESSAGE(eth.isLinked(), "no Ethernet link, check cable");
}

static void test_dhcp_lease_obtained() {
    TEST_ASSERT_TRUE_MESSAGE(ethStarted, "skipped: W5500 not started");
    uint32_t start = millis();
    while (!eth.connected() && millis() - start < DHCP_TIMEOUT_MS) delay(100);
    TEST_ASSERT_TRUE_MESSAGE(eth.connected(), "no DHCP lease within timeout");

    IPAddress ip = eth.localIP();
    TEST_ASSERT_NOT_EQUAL(0, (uint32_t)ip);
    TEST_ASSERT_TRUE_MESSAGE(eth.gatewayIP().isSet(), "lease has no gateway");
    Serial.print("Lease: ");
    Serial.println(ip);
}

static void test_gateway_answers_ping() {
    TEST_ASSERT_TRUE_MESSAGE(eth.connected(), "skipped: no lease");
    int ttl = eth.ping(eth.gatewayIP(), 64, 2000);
    TEST_ASSERT_TRUE_MESSAGE(ttl >= 0, "gateway did not answer ping");
}

void setup() {
    // The boot program starts every image with the watchdog armed (~8.3 s);
    // these tests run longer than that in setup() and do not feed it
    watchdog_disable();
    Serial.begin(115200);
    uint32_t start = millis();
    while (!Serial && millis() - start < 10000) delay(10);
    delay(500);

    UNITY_BEGIN();
    RUN_TEST(test_w5500_detected);
    RUN_TEST(test_link_comes_up);
    RUN_TEST(test_dhcp_lease_obtained);
    RUN_TEST(test_gateway_answers_ping);
    UNITY_END();
}

void loop() {}

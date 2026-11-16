// Channel scanner: find which 2.4 GHz channel(s) the remote transmits on.
//
// Sweeps all 126 nRF24 channels (2400-2525 MHz) using the Received Power
// Detector (testRPD: signal stronger than about -64 dBm). Works regardless of
// data rate, address, or packet format. Keep the remote close (< 30 cm).
//
// Output, once per second:  "S <seconds> <sweeps> ch:hits ch:hits ..."
// (only channels with at least one hit are listed)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <RF24.h>

RF24 radio(RF_CE_PIN, RF_CS_PIN);

static const uint8_t NUM_CHANNELS = 126;
uint16_t hits[NUM_CHANNELS];
uint32_t sweeps = 0;
uint32_t lastReport = 0;

void setup() {
  // Make sure the ESP8266's own Wi-Fi isn't transmitting and polluting the scan.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println(F("=== nRF24 channel scanner ==="));
  if (!radio.begin()) {
    Serial.println(F("radio.begin() FAILED - check wiring"));
    while (true) delay(1000);
  }
  radio.setAutoAck(false);
  radio.setDataRate(RF24_1MBPS);
  radio.startListening();
  radio.stopListening();
  Serial.println(F("Scanning channels 0-125. Press buttons on the remote..."));
}

void loop() {
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    radio.setChannel(ch);
    radio.startListening();
    delayMicroseconds(170);   // > 130us settle + RPD measurement window
    radio.stopListening();
    if (radio.testRPD()) hits[ch]++;
  }
  sweeps++;

  uint32_t now = millis();
  if (now - lastReport >= 1000) {
    lastReport = now;
    Serial.printf("S %lu %u", now / 1000, sweeps);
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      if (hits[ch]) Serial.printf(" %u:%u", ch, hits[ch]);
      hits[ch] = 0;
    }
    Serial.println();
    sweeps = 0;
  }
}

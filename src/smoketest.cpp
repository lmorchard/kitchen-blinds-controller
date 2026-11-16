// SPI smoke test: confirm the ESP8266 can talk to the nRF24L01+.
//
// Wiring (matches RFPowerView's example pins):
//   nRF24  -> D1 Mini
//   GND    -> G
//   VCC    -> 3V3 (3.3V only!)
//   CE     -> D1 (GPIO5)
//   CSN    -> D8 (GPIO15)
//   SCK    -> D5 (GPIO14)
//   MOSI   -> D7 (GPIO13)
//   MISO   -> D6 (GPIO12)
//   IRQ    -> D2 (GPIO4)  (not used by this test, required by RFPowerView)

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define RF_CE_PIN  5   // D1
#define RF_CS_PIN  15  // D8
#define RF_IRQ_PIN 4   // D2

RF24 radio(RF_CE_PIN, RF_CS_PIN);

void runCheck() {
  Serial.println();
  Serial.println(F("=== nRF24L01+ SPI smoke test ==="));

  bool began = radio.begin();
  Serial.printf("radio.begin():         %s\n", began ? "OK" : "FAILED");
  Serial.printf("isChipConnected():     %s\n", radio.isChipConnected() ? "YES" : "NO");
  Serial.printf("isPVariant() (L01+):   %s\n", radio.isPVariant() ? "YES" : "NO");

  // Write/read-back test: set a known channel and read it back over SPI.
  radio.setChannel(7);  // channel PowerView uses
  uint8_t ch = radio.getChannel();
  Serial.printf("channel write/read 7:  %u %s\n", ch, ch == 7 ? "(OK)" : "(MISMATCH)");

  pinMode(RF_IRQ_PIN, INPUT);
  Serial.printf("IRQ pin (D2) level:    %s (idle should be HIGH)\n",
                digitalRead(RF_IRQ_PIN) ? "HIGH" : "LOW");

  Serial.println(F("--- printPrettyDetails() ---"));
  radio.printPrettyDetails();
  Serial.println(F("=== done ==="));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  runCheck();
}

void loop() {
  // Re-run every 10s so the monitor always has fresh output.
  delay(10000);
  runCheck();
}

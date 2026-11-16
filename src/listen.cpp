// Listener for our PowerView network: tunes directly to the known radio
// address, validates packets with the corrected CRC, and prints one line
// per button press.
//
// CRC = CRC-16, poly 0x755B, init 0xFFFF, over [rfID hi, rfID lo] + packet.
// (See docs/protocol-notes.md; RFPowerView v0.0.3 gets this wrong.)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <RF24.h>

#ifndef RF_ID
#define RF_ID 0x0817
#endif

RF24 radio(RF_CE_PIN, RF_CS_PIN);

static const uint8_t PAYLOAD = 32;
static const uint32_t PRESS_GAP_MS = 700;  // repeats closer than this = same press

uint16_t crc16(const uint8_t *data, uint8_t len, uint16_t crc) {
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x755B : crc << 1;
  }
  return crc;
}

uint16_t crcInit;  // CRC state after the 2 address bytes

bool crcOk(const uint8_t *p) {
  uint8_t len = p[1];
  if (len < 0x0A || len > 28) return false;
  uint16_t c = crc16(p, len + 2, crcInit);
  return p[len + 2] == (c >> 8) && p[len + 3] == (c & 0xFF);
}

const char *commandName(uint8_t type, uint8_t cmd) {
  if (type != 0x52) return "?";
  switch (cmd) {
    case 0x55: return "OPEN";
    case 0x44: return "CLOSE";
    case 0x53: return "STOP";
    case 0x52: return "OPEN_SLOW";
    case 0x4C: return "CLOSE_SLOW";
    case 0x48: return "FAVORITE (move to saved position)";
    default:   return "?";
  }
}

// Current press being accumulated
uint32_t pressCount = 0;
uint32_t lastPacketMs = 0;
uint8_t pressPacket[32];
uint8_t pressLen = 0;
uint16_t repeats = 0;
uint8_t firstRc1 = 0, lastRc1 = 0;
bool inPress = false;

void printHex(const uint8_t *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) Serial.printf("%02X", b[i]);
}

void finishPress() {
  if (!inPress) return;
  inPress = false;
  pressCount++;
  const uint8_t *p = pressPacket;
  uint8_t len = pressLen;

  Serial.printf("#%u ", pressCount);
  uint8_t off = 12;
  if (p[10] == 0x06) {
    Serial.print("groups:");
    while (off < len - 2 && p[off] != 0x00) Serial.printf(" %u", p[off++]);
    off++;
    Serial.printf("  source: 0x%02X%02X", p[off], p[off + 1]);
    off += 2;
  } else {
    Serial.printf("address type 0x%02X", p[10]);
    off = 255;
  }
  if (off < len - 3) {
    Serial.printf("  command: %s (%02X %02X)", commandName(p[off], p[off + 1]), p[off], p[off + 1]);
  }
  Serial.printf("  repeats: %u  rolling code 1: %02X..%02X\n", repeats, firstRc1, lastRc1);
  Serial.print("    first packet: ");
  printHex(p, len);
  Serial.println();
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.printf("=== PowerView listener, RF_ID=0x%04X ===\n", RF_ID);

  const uint8_t addrBytes[2] = {(uint8_t)(RF_ID >> 8), (uint8_t)(RF_ID & 0xFF)};
  crcInit = crc16(addrBytes, 2, 0xFFFF);
  Serial.printf("CRC init for this network: 0x%04X\n", crcInit);

  // Self-test against a captured STOP packet from our remote.
  const uint8_t sample[] = {0xC0, 0x11, 0x00, 0x05, 0x61, 0xFF, 0xFF, 0x1D, 0x84, 0x86, 0x06,
                            0x11, 0x01, 0x00, 0x1D, 0x84, 0x52, 0x53, 0x00, 0xB1, 0x6C};
  Serial.printf("CRC self-test on captured packet: %s\n", crcOk(sample) ? "PASS" : "FAIL");

  if (!radio.begin()) {
    Serial.println(F("radio.begin() FAILED - check wiring"));
    while (true) delay(1000);
  }
  radio.setChannel(7);
  radio.setDataRate(RF24_1MBPS);
  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setRetries(0, 0);
  radio.setPayloadSize(PAYLOAD);
  radio.setAddressWidth(2);
  // Register bytes are LSByte first; on air the MSByte goes first.
  const uint8_t pipeAddr[2] = {(uint8_t)(RF_ID & 0xFF), (uint8_t)(RF_ID >> 8)};
  radio.openReadingPipe(1, pipeAddr);
  radio.startListening();
  Serial.println(F("Listening. Press buttons on the remote..."));
}

void loop() {
  uint8_t raw[PAYLOAD];
  while (radio.available()) {
    radio.read(raw, PAYLOAD);
    if (raw[0] != 0xC0 || !crcOk(raw)) continue;

    uint32_t now = millis();
    uint8_t len = raw[1] + 4;
    if (inPress && now - lastPacketMs > PRESS_GAP_MS) finishPress();
    if (!inPress) {
      inPress = true;
      memcpy(pressPacket, raw, len);
      pressLen = len;
      repeats = 0;
      firstRc1 = raw[4];
    }
    repeats++;
    lastRc1 = raw[4];
    lastPacketMs = now;
  }
  if (inPress && millis() - lastPacketMs > PRESS_GAP_MS) finishPress();
}

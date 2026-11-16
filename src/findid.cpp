// RF_ID finder: deterministic search for a PowerView remote's 2-byte radio
// address on channel 7, without a PowerView Hub.
//
// On air, an nRF24 frame is:  [preamble][addr MSByte][addr LSByte][payload]
// The preamble is 0xAA if the first address bit is 1, else 0x55. So if we
// listen with a 2-byte address of [preamble][guess], a correct guess for the
// address high byte yields frames that begin with the address low byte,
// followed by a PowerView packet (0xC0 len 0x00 0x05 ... CRC).
//
// The nRF24 has 6 RX pipes. Pipes 1-5 share their high byte (the preamble)
// and differ in the low byte (our guess), and pipe 0 gets a sixth guess. We
// cycle through all 256 guesses (preamble chosen per guess) several times per
// second, and also try other data rates in case the remote isn't at 1 Mbps.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPI.h>
#include <RF24.h>
#include <PacketCRC.h>

RF24 radio(RF_CE_PIN, RF_CS_PIN);
PacketCRC packetCRC;

static const uint8_t PAYLOAD = 32;
static const uint32_t DWELL_US = 2500;  // time spent listening per set of 6 guesses

const rf24_datarate_e RATES[] = {RF24_1MBPS, RF24_1MBPS, RF24_2MBPS, RF24_250KBPS};
const char *RATE_NAMES[] = {"1Mbps", "1Mbps", "2Mbps", "250kbps"};
const uint8_t NUM_RATES = sizeof(RATES) / sizeof(RATES[0]);

uint8_t rateIdx = 0;
uint16_t nextGuess = 0;   // 0..255 within the current preamble group
uint8_t guesses[6];
uint8_t preamble = 0x55;
uint32_t frames = 0, matches = 0, nearMisses = 0, cycles = 0;
uint32_t lastStatus = 0;

void printHex(const uint8_t *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) Serial.printf("%02X", b[i]);
}

const char *commandName(uint8_t type, uint8_t cmd) {
  if (type != 0x52) return "?";
  switch (cmd) {
    case 0x55: return "OPEN";
    case 0x44: return "CLOSE";
    case 0x53: return "STOP";
    case 0x52: return "OPEN_SLOW";
    case 0x4C: return "CLOSE_SLOW";
    case 0x48: return "MOVE_TO_SAVED_POSITION (favorite)";
    default:   return "?";
  }
}

// Load the next 6 guesses. Guesses 0x00-0x7F use preamble 0x55, 0x80-0xFF use
// 0xAA, so a batch never straddles the two groups (pipes 1-5 share a preamble).
void loadNextBatch() {
  if (nextGuess >= 256) {
    nextGuess = 0;
    rateIdx = (rateIdx + 1) % NUM_RATES;
    if (rateIdx == 0) cycles++;
  }
  preamble = (nextGuess < 0x80) ? 0x55 : 0xAA;
  uint16_t groupEnd = (nextGuess < 0x80) ? 0x80 : 0x100;
  for (uint8_t n = 0; n < 6; n++) {
    if (nextGuess < groupEnd) {
      guesses[n] = (uint8_t)nextGuess++;
    } else {
      guesses[n] = guesses[n - 1];  // pad a short final batch (n >= 1 here)
    }
  }

  radio.stopListening();
  radio.setDataRate(RATES[rateIdx]);
  // Register bytes are LSByte first; the MSByte (preamble) goes on air first.
  uint8_t a0[2] = {guesses[0], preamble};
  uint8_t a1[2] = {guesses[1], preamble};
  radio.openReadingPipe(0, a0);
  radio.openReadingPipe(1, a1);
  for (uint8_t p = 2; p < 6; p++) {
    uint8_t a[2] = {guesses[p], preamble};
    radio.openReadingPipe(p, a);
  }
  radio.startListening();
  radio.flush_rx();
}

void report(uint8_t hi, uint8_t pipe, const uint8_t *raw, bool crcOk) {
  const uint8_t lo = raw[0];
  const uint8_t *p = raw + 1;
  uint8_t len = p[1] + 4;

  Serial.println();
  Serial.printf("*** %s at %s: RF_ID=0x%02X%02X (pipe %u, preamble %02X)\n",
                crcOk ? "MATCH" : "near-miss (CRC failed)", RATE_NAMES[rateIdx], hi, lo, pipe, preamble);
  Serial.print("    frame:  ");
  printHex(raw, PAYLOAD);
  Serial.println();
  if (!crcOk) return;

  Serial.print("    packet: ");
  printHex(p, len);
  Serial.println();
  Serial.printf("    rolling codes: %02X %02X   physical source: 0x%02X%02X\n", p[4], p[11], p[7], p[8]);
  uint8_t off = 12;
  if (p[10] == 0x06) {
    Serial.print("    GROUPS:");
    while (off < len - 2 && p[off] != 0x00) Serial.printf(" %u", p[off++]);
    off++;
    Serial.printf("  logical source: 0x%02X%02X\n", p[off], p[off + 1]);
    off += 2;
  } else {
    Serial.printf("    address type 0x%02X\n", p[10]);
    off = 255;
  }
  if (off < len - 3) {
    Serial.printf("    command: %s (%02X %02X)\n", commandName(p[off], p[off + 1]), p[off], p[off + 1]);
  }
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println(F("=== PowerView RF_ID finder (deterministic search) ==="));

  const uint8_t sample[] = {0xC0, 0x11, 0x00, 0x05, 0x6C, 0xFF, 0xFF, 0x36, 0x9E, 0x86, 0x06,
                            0x3C, 0x04, 0x00, 0x36, 0x9E, 0x52, 0x55, 0x00, 0xB9, 0x88};
  Serial.printf("CRC self-test on README sample: %s\n", packetCRC.check(sample) ? "PASS" : "FAIL");

  if (!radio.begin()) {
    Serial.println(F("radio.begin() FAILED - check wiring"));
    while (true) delay(1000);
  }
  radio.setChannel(7);
  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setRetries(0, 0);
  radio.setPayloadSize(PAYLOAD);
  radio.setAddressWidth(2);
  loadNextBatch();
  Serial.println(F("Searching on channel 7. Press Stop on the remote, every few seconds..."));
}

void loop() {
  uint32_t start = micros();
  while (micros() - start < DWELL_US) {
    uint8_t pipe;
    if (radio.available(&pipe)) {
      uint8_t raw[PAYLOAD];
      radio.read(raw, PAYLOAD);
      frames++;
      uint8_t hi = guesses[pipe];
      const uint8_t *p = raw + 1;
      // raw[0] = address low byte, raw[1..] should be a PowerView packet.
      if (p[0] == 0xC0 && p[2] == 0x00 && p[3] == 0x05 && p[1] >= 0x0A && p[1] <= 26) {
        bool ok = packetCRC.check(p);
        if (ok) matches++; else nearMisses++;
        if (ok || nearMisses <= 20) report(hi, pipe, raw, ok);
      }
    }
  }
  loadNextBatch();

  uint32_t now = millis();
  if (now - lastStatus >= 10000) {
    lastStatus = now;
    Serial.printf("[status] %lus up, %u full cycles, %u frames, %u matches, %u near-misses\n",
                  now / 1000, cycles, frames, matches, nearMisses);
  }
}

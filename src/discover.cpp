// Discovery sniffer: find a PowerView remote's 2-byte radio address (RF_ID)
// without a PowerView Hub.
//
// RFPowerView only receives on a known RF_ID. To learn it, we put the nRF24
// into "promiscuous" mode (Travis Goodspeed's trick, see
// https://travisgoodspeed.blogspot.com/2011/02/promiscuity-is-nrf24l01s-duty.html): a 2-byte address of
// 0x00AA / 0x0055 matches the radio's preamble after a run of noise, CRC is
// disabled, and we capture raw 32-byte payloads. Most captures are noise; real
// PowerView packets are found by scanning every bit offset for a 0xC0 header
// with a valid RFPowerView CRC. The 2 bytes just before 0xC0 are the radio
// address the remote transmitted to.

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <PacketCRC.h>

RF24 radio(RF_CE_PIN, RF_CS_PIN);
PacketCRC packetCRC;

static const uint8_t PAYLOAD = 32;

uint32_t rawCount = 0;
uint32_t hitCount = 0;
uint32_t lastStatus = 0;
uint8_t lastPacket[32];
uint8_t lastPacketLen = 0;
uint32_t repeatCount = 0;

void printHex(const uint8_t *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) Serial.printf("%02X", b[i]);
}

// Shift a buffer left by `bits` (0-7) into out (len-1 bytes valid).
void shiftLeft(const uint8_t *in, uint8_t *out, uint8_t len, uint8_t bits) {
  for (uint8_t i = 0; i < len - 1; i++) {
    out[i] = bits ? (uint8_t)((in[i] << bits) | (in[i + 1] >> (8 - bits))) : in[i];
  }
  out[len - 1] = bits ? (uint8_t)(in[len - 1] << bits) : in[len - 1];
}

const char *commandName(uint8_t type, uint8_t cmd) {
  if (type != 0x52) return type == 0x3F ? "fields-command" : type == 0x21 ? "fields-report" : "?";
  switch (cmd) {
    case 0x55: return "OPEN";
    case 0x44: return "CLOSE";
    case 0x53: return "STOP";
    case 0x52: return "OPEN_SLOW";
    case 0x4C: return "CLOSE_SLOW";
    case 0x48: return "MOVE_TO_SAVED_POSITION";
    default:   return "?";
  }
}

void reportPacket(const uint8_t *addr, const uint8_t *p, uint8_t pipe, uint8_t bits) {
  uint8_t len = p[1] + 4;

  // Collapse the remote's many repeats of the same packet into one line.
  if (len == lastPacketLen && memcmp(p, lastPacket, len) == 0) {
    repeatCount++;
    return;
  }
  if (repeatCount) Serial.printf("   (previous packet repeated %u more times)\n", repeatCount);
  repeatCount = 0;
  memcpy(lastPacket, p, len);
  lastPacketLen = len;
  hitCount++;

  Serial.println();
  Serial.printf("*** PowerView packet #%u (pipe %u, bit shift %u)\n", hitCount, pipe, bits);
  if (addr) {
    Serial.printf("    radio address on air: %02X %02X  ->  try RF_ID=0x%02X%02X\n",
                  addr[0], addr[1], addr[0], addr[1]);
  } else {
    Serial.println("    radio address: (not captured in this frame)");
  }
  Serial.print("    packet: ");
  printHex(p, len);
  Serial.println();

  uint16_t phys = (p[7] << 8) | p[8];
  uint8_t addrType = p[10];
  Serial.printf("    rolling codes: %02X %02X   physical source: 0x%04X\n", p[4], p[11], phys);

  uint8_t off = 12;
  if (addrType == 0x06) {
    Serial.print("    address type: GROUPS  groups:");
    while (off < len - 2 && p[off] != 0x00) Serial.printf(" %u", p[off++]);
    off++;  // terminator
    Serial.printf("  logical source: 0x%04X\n", (p[off] << 8) | p[off + 1]);
    off += 2;
  } else if (addrType == 0x05) {
    Serial.printf("    address type: UNICAST  dest: 0x%04X  source: 0x%04X\n",
                  (p[off] << 8) | p[off + 1], (p[off + 2] << 8) | p[off + 3]);
    off += 4;
  } else if (addrType == 0x04) {
    Serial.printf("    address type: BROADCAST  source: 0x%04X\n", (p[off] << 8) | p[off + 1]);
    off += 2;
  } else {
    Serial.printf("    address type: 0x%02X (unknown)\n", addrType);
  }
  if (off + 1 < len - 2) {
    Serial.printf("    command: %s (%02X %02X)\n", commandName(p[off], p[off + 1]), p[off], p[off + 1]);
  }
}

void scan(const uint8_t *raw, uint8_t pipe) {
  uint8_t buf[PAYLOAD];
  for (uint8_t bits = 0; bits < 8; bits++) {
    shiftLeft(raw, buf, PAYLOAD, bits);
    uint8_t usable = PAYLOAD - (bits ? 1 : 0);
    for (uint8_t i = 0; i + 4 <= usable; i++) {
      if (buf[i] != 0xC0) continue;
      uint8_t plen = buf[i + 1];
      if (plen < 0x0A || plen > 28) continue;
      if (i + plen + 4 > usable) continue;
      if (buf[i + 3] != 0x05) continue;
      if (!packetCRC.check(&buf[i])) continue;
      reportPacket(i >= 2 ? &buf[i - 2] : nullptr, &buf[i], pipe, bits);
      return;
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println(F("=== PowerView RF_ID discovery sniffer ==="));

  // Self-test: "Open group 4" sample packet from the RFPowerView README.
  const uint8_t sample[] = {0xC0, 0x11, 0x00, 0x05, 0x6C, 0xFF, 0xFF, 0x36, 0x9E, 0x86, 0x06,
                            0x3C, 0x04, 0x00, 0x36, 0x9E, 0x52, 0x55, 0x00, 0xB9, 0x88};
  Serial.printf("CRC self-test on README sample: %s\n", packetCRC.check(sample) ? "PASS" : "FAIL");

  if (!radio.begin()) {
    Serial.println(F("radio.begin() FAILED - check wiring"));
    while (true) delay(1000);
  }
  radio.setChannel(7);              // PowerView channel
  radio.setDataRate(RF24_1MBPS);    // PowerView data rate
  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setRetries(0, 0);
  radio.setPayloadSize(PAYLOAD);
  radio.setAddressWidth(2);         // undocumented 2-byte address width

  // Register bytes are LSByte first; on air they're sent MSByte first,
  // so these match "00 AA" / "00 55" (noise then preamble).
  const uint8_t addrAA[2] = {0xAA, 0x00};
  const uint8_t addr55[2] = {0x55, 0x00};
  radio.openReadingPipe(0, addrAA);
  radio.openReadingPipe(1, addr55);
  radio.startListening();

  Serial.println(F("Listening on channel 7. Press buttons on the remote..."));
}

void loop() {
  uint8_t pipe;
  uint8_t raw[PAYLOAD];
  while (radio.available(&pipe)) {
    radio.read(raw, PAYLOAD);
    rawCount++;
#ifdef RAW_DUMP
    // Dump every raw frame for offline analysis: "R <ms> <pipe> <hex>"
    Serial.printf("R %lu %u ", millis(), pipe);
    printHex(raw, PAYLOAD);
    Serial.println();
#endif
    scan(raw, pipe);
  }

  uint32_t now = millis();
  if (now - lastStatus >= 10000) {
    lastStatus = now;
    if (repeatCount) {
      Serial.printf("   (previous packet repeated %u more times)\n", repeatCount);
      repeatCount = 0;
    }
    Serial.printf("[status] %lus up, %u raw frames captured, %u PowerView packets\n",
                  now / 1000, rawCount, hitCount);
  }
}

// Transmit test: send PowerView commands to our blind, one keystroke at a time.
// Nothing is transmitted unless a key is received over serial:
//   s = STOP   o = OPEN   f = FAVORITE (move to saved position)
//
// Also prints any packets received (e.g. from the real remote) using the
// patched RFPowerView library, which confirms the library's RX path and CRC.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <RFPowerView.h>
#include <BufferFiller.h>
#include <PacketCRC.h>

RFPowerView powerView(RF_CE_PIN, RF_CS_PIN, RF_IRQ_PIN, RF_ID);

void printHex(const uint8_t *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) Serial.printf("%02X", b[i]);
}

const char *typeName(PacketType t) {
  switch (t) {
    case PacketType::OPEN: return "OPEN";
    case PacketType::CLOSE: return "CLOSE";
    case PacketType::STOP: return "STOP";
    case PacketType::OPEN_SLOW: return "OPEN_SLOW";
    case PacketType::CLOSE_SLOW: return "CLOSE_SLOW";
    case PacketType::MOVE_TO_SAVED_POSITION: return "FAVORITE";
    default: return "other";
  }
}

Packet makePacket(PacketType type, uint8_t rc1, uint8_t rc2) {
  Packet packet;
  GroupsHeader header;
  header.source = REMOTE_ID;
  header.groups = {BLIND_GROUP};
  packet.header = header;
  packet.type = type;
  packet.parameters = std::monostate{};
  packet.rollingCode1 = rc1;
  packet.rollingCode2 = rc2;
  return packet;
}

// Build a packet the same way RFPowerView will, and show its bytes.
void showPacket(const Packet &packet, uint8_t *buf) {
  BufferFiller filler(0x05);
  memset(buf, 0, 32);
  filler.fill(buf, &packet);
  printHex(buf, buf[1] + 4);
}

void selfTest() {
  // Rebuild a STOP packet captured from the real remote and compare.
  const char *expected = "C011000561FFFF1D8486061101001D84525300B16C";
  uint8_t buf[32];
  Packet p = makePacket(PacketType::STOP, 0x61, 0x11);
  Serial.print(F("Self-test: built   "));
  showPacket(p, buf);
  Serial.println();
  Serial.printf("           captured %s\n", expected);
  char hex[64] = {0};
  for (uint8_t i = 0; i < buf[1] + 4; i++) sprintf(hex + i * 2, "%02X", buf[i]);
  Serial.printf("           %s\n", strcmp(hex, expected) == 0 ? "MATCH - packet builder and CRC are correct"
                                                               : "MISMATCH - do not transmit!");
}

void onPacket(const Packet *packet) {
  Serial.print(F("RX: "));
  if (std::holds_alternative<GroupsHeader>(packet->header)) {
    auto h = std::get<GroupsHeader>(packet->header);
    Serial.printf("source 0x%04X groups:", h.source);
    for (auto g : h.groups) Serial.printf(" %u", g);
  }
  Serial.printf("  %s  rolling codes %02X %02X\n", typeName(packet->type), packet->rollingCode1,
                packet->rollingCode2);
}

void send(PacketType type) {
  uint8_t rc1 = (uint8_t)random(256);
  uint8_t rc2 = (uint8_t)random(256);
  Packet p = makePacket(type, rc1, rc2);
  uint8_t buf[32];
  Serial.printf("TX: %s to group %u ... packet ", typeName(type), BLIND_GROUP);
  showPacket(p, buf);
  Serial.println();
  uint32_t t0 = millis();
  bool ok = powerView.sendPacket(&p);
  Serial.printf("    %s in %lu ms\n", ok ? "sent" : "FAILED to build packet", (unsigned long)(millis() - t0));
}

void setup() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.printf("=== PowerView transmit test: RF_ID=0x%04X remote=0x%04X group=%u ===\n", RF_ID, REMOTE_ID,
                BLIND_GROUP);
  randomSeed(ESP.getCycleCount() ^ micros());
  selfTest();

  powerView.setPacketReceivedCallback(onPacket);
  if (!powerView.begin()) {
    Serial.println(F("RFPowerView begin() FAILED - check wiring"));
    while (true) delay(1000);
  }
  Serial.println(F("Ready. Keys: s = STOP, o = OPEN, f = FAVORITE"));
}

void loop() {
  powerView.loop();
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 's': send(PacketType::STOP); break;
      case 'o': send(PacketType::OPEN); break;
      case 'f': send(PacketType::MOVE_TO_SAVED_POSITION); break;
      case '\r': case '\n': case ' ': break;
      default: Serial.printf("Unknown key '%c'. Keys: s = STOP, o = OPEN, f = FAVORITE\n", c);
    }
  }
}

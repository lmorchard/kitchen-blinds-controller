#include "PacketCRC.h"

// See PacketCRC.h for the local patch notes.

static const uint16_t POLYNOMIAL = 0x755B;
static const uint16_t DEFAULT_RF_ID = 0x2EC8;  // upstream author's network

uint16_t PacketCRC::initState = 0xF48B;  // == state after DEFAULT_RF_ID

PacketCRC::PacketCRC() {}

PacketCRC::~PacketCRC() {}

uint16_t PacketCRC::update(uint16_t crc, const uint8_t* data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ POLYNOMIAL) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

void PacketCRC::setNetworkAddress(uint16_t rfID) {
  const uint8_t address[2] = {(uint8_t)(rfID >> 8), (uint8_t)(rfID & 0xFF)};
  initState = update(0xFFFF, address, 2);
}

bool PacketCRC::check(const uint8_t* packet) {
  uint8_t length = packet[1];

  if (length <= 0 or length > 28) {
    return false;
  }

  uint16_t result = calculate(packet);
  uint8_t checksum1 = (uint8_t)((result & 0xFF00) >> 8);
  uint8_t checksum2 = (uint8_t)(result & 0x00FF);

  return packet[length + 2] == checksum1 && packet[length + 3] == checksum2;
}

uint16_t PacketCRC::calculate(const uint8_t* packet) {
  uint8_t length = packet[1];
  return update(initState, packet, length + 2);
}

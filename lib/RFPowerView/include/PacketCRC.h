#ifndef PACKET_CRC_H
#define PACKET_CRC_H

#include <stdint.h>

// PowerView packet CRC: CRC-16, polynomial 0x755B, no reflection, xorout 0,
// init 0xFFFF, computed over the 2-byte radio address (rfID, high byte first)
// followed by the packet.
//
// LOCAL PATCH (kitchen-blinds-controller): upstream v0.0.3 hard-coded
// CRC16(0x755B, 0xF48B) over the packet only. 0xF48B is the CRC state after
// the upstream author's address 0x2EC8, so it only worked on that network.
// The address is now configurable; RFPowerView's constructor sets it.
// It defaults to 0x2EC8 so existing behaviour is unchanged.
class PacketCRC {
public:
    PacketCRC();
    ~PacketCRC();

    // Set the radio address used for all PacketCRC instances.
    static void setNetworkAddress(uint16_t rfID);

    bool check(const uint8_t* packet);
    uint16_t calculate(const uint8_t* packet);

private:
    static uint16_t initState;  // CRC state after feeding the address bytes
    static uint16_t update(uint16_t crc, const uint8_t* data, uint8_t len);
};

#endif // PACKET_CRC_H

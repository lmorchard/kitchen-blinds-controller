# PowerView Gen 2 remote: findings

Remote: Hunter Douglas Pebble, FCC ID `UXURC4U4`. Captured 2026-11-15 with
`src/findid.cpp` (env `findid`).

## Our network

| Item | Value |
|---|---|
| Channel | 7 (2407 MHz), confirmed with `src/scanner.cpp` |
| Data rate | 1 Mbps, no nRF24 CRC, no auto-ack |
| Radio address (`RF_ID`) | **`0x0817`** (2-byte address, on air `08 17`, preamble `0x55`) |
| Remote ID (physical + logical source) | **`0x1D84`** |
| Group (kitchen blind) | **1** |
| Address type | `0x06` (groups) |

## Sample packets (STOP, group 1)

```
c011000561ffff1d8486061101001d84525300b16c
c011000562ffff1d8486061201001d84525300c1f3
c011000563ffff1d8486061301001d845253003d4f
c01100051affff1d8486060601001d84525300de72
```

Breaking down the first one:

```
C0          header
11          length (bytes after this one, excluding the 2 CRC bytes)
00 05       constants
61          rolling code 1  (increments on every transmitted repeat)
FF FF       constants
1D 84       physical source (remote ID)
86          constant
06          address type: groups
11          rolling code 2  (increments alongside rolling code 1)
01 00       group 1, terminator
1D 84       logical source (remote ID)
52 53 00    command: STOP (52 55 = OPEN, 52 44 = CLOSE, 52 48 = favorite/saved position)
B1 6C       CRC
```

## CRC: RFPowerView v0.0.3 only works for its author's network

The packet CRC is CRC-16, polynomial `0x755B`, no reflection, xorout `0x0000`,
**init `0xFFFF`, computed over the 2 radio-address bytes followed by the packet**
(header through the byte before the CRC):

```
CRC = crc16(0x755B, init=0xFFFF, data = [rfID_hi, rfID_lo] + packet[0 .. len+1])
```

RFPowerView hard-codes `CRC16(0x755B, 0xF48B, ...)` over the packet only.
`0xF48B` is the CRC state after feeding the author's address `2E C8`, so the
library rejects (and generates wrong CRCs for) every other network.

For our network the equivalent init value is **`0x0823`**, i.e. the state after
feeding `08 17`. This was verified against all captured packets and against the
README sample packet (with rfID `0x2EC8`).

To use RFPowerView we must patch `PacketCRC` to derive its init from the rfID
(or hard-code `0x0823`).

# Local copy of RFPowerView

Vendored from https://github.com/mattyway/RFPowerView at tag `v0.0.3`
(GPL-2.0-only, see LICENSE), with one local patch:

- **PacketCRC**: the CRC is computed over the 2-byte radio address plus the
  packet (CRC-16, poly 0x755B, init 0xFFFF). Upstream hard-coded the CRC state
  for its author's address (0x2EC8), so it rejected and mis-signed packets on
  every other network. `RFPowerView`'s constructor now calls
  `PacketCRC::setNetworkAddress(rfID)`. This also removes the dependency on
  `robtillaart/CRC@1.0.2`, which is no longer in the PlatformIO registry.

See `docs/protocol-notes.md` in the project root for how this was worked out.

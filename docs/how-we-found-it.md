# How we worked out the remote's protocol

This is how we went from "there's an nRF24 radio in this remote" to sending
working commands to the blind, without a PowerView Hub. The reference details
(IDs, packet layout, captured packets) are in
[protocol-notes.md](protocol-notes.md).

## 1. Starting point

The Pebble remote's FCC filing (`UXURC4U4`) and the
[RFPowerView](https://github.com/mattyway/RFPowerView) library said the remote
uses a Nordic **nRF24**-style radio, the same chip family as our module. The
library also gave the basics: **channel 7** (2407 MHz), **1 Mbps**, and the
layout of a PowerView packet.

The catch: an nRF24 radio only receives packets sent to a specific
**address**, and PowerView uses a 2-byte one. RFPowerView's author read his
from a PowerView Hub's API. We didn't have a hub, so we had to find it
another way.

## 2. First attempt: listen to everything (`discover`)

A known trick (from Travis Goodspeed's ["Promiscuity is the nRF24L01+'s Duty"](https://travisgoodspeed.blogspot.com/2011/02/promiscuity-is-nrf24l01s-duty.html), 2011) makes an nRF24 accept almost anything:
give it a tiny 2-byte address that matches the start of every transmission
(`00 AA` / `00 55`, i.e. silence followed by the lead-in byte), turn off its
error checking, and save whatever comes in. Most of it is random noise, so
the sniffer searched each capture, at every bit offset, for the PowerView
packet layout.

It caught nothing, across two captures. A dump of every raw frame during
button presses (`discover_raw`) showed only background noise. At that point
we couldn't tell whether the trick was failing or we were on the wrong
channel.

## 3. Checking the channel (`scanner`)

The nRF24 has a simple signal-strength detector (`testRPD()`: is there a
signal stronger than about -64 dBm?). We swept it across all 126 channels
about 12 times a second while pressing Stop. Seven short bursts appeared on
**channels 6–8**, centred on 7, at the moments the 7 buttons were pressed.

That proved the channel was right and the listen-to-everything trick was the
problem: it relies on luck that a short button press doesn't give it.

## 4. Finding the address by elimination (`findid`)

Every nRF24 transmission starts the same way:

```
[lead-in byte][address byte 1][address byte 2][packet: C0 ...]
```

The lead-in byte is fixed by the address's first bit: `AA` if it's 1, `55`
if it's 0. So if we tell the radio to listen for **lead-in + a guess at
address byte 1**, a correct guess makes it receive the rest: **address byte
2**, then a packet starting `C0 11 00 05`.

There are only 256 possible guesses, and the radio can listen on 6 addresses
at once (its 6 receive pipes), so we cycled through all of them several
times a second while pressing Stop. One guess, `08`, produced frames starting
`17 C0 11 00 05 ...`. That gave us the address **`0x0817`**, and the packet
behind it matched RFPowerView's layout:

```
C0 11 00 05 61 FF FF 1D 84 86 06 11 01 00 1D 84 52 53 00 B1 6C
                     └src┘    grp ┘ └─┘ └src┘ └STOP┘    └CRC┘
```

So: **remote ID `0x1D84`**, **group 1**, **Stop = `52 53`**.

## 5. The checksum puzzle

Each packet ends with a 2-byte CRC, and ours **failed** RFPowerView's check
every time, even though everything else in the packets made sense.

RFPowerView's CRC is CRC-16 with polynomial `0x755B` and a fixed starting
value. We kept the polynomial and tried all 65,536 possible starting values
against the captured packets. Exactly one worked, `0x0823`, where the library
uses `0xF48B`.

That raised the question of why the starting values differ. A starting value
like that is usually the CRC's state after some data has already been fed in.
We tested the guess that **the radio address is included** in the CRC, with
the standard starting value `0xFFFF`:

- It fit all of our captured packets (address `08 17`).
- It also fit the example packet in RFPowerView's README when given `2E C8`,
  the address in the author's PowerViewSniffer config.

That confirmed the real algorithm, and exposed the library bug below.

## 6. Reading the other buttons (`listen`)

With the address and the correct CRC, a simple listener could decode every
press cleanly. It confirmed **Open (`52 55`)** and **Favorite (`52 48`)**, and
showed that:

- each press sends the **same packet about 645 times**;
- the two "rolling code" bytes change between presses, but not between
  repeats, and not as a simple counter.

## 7. Proving it by transmitting (`txtest`)

Before sending anything, the patched library rebuilt one of the captured Stop
packets from scratch, and it came out byte-for-byte identical, CRC included.
Then we sent real commands: Stop, Favorite and Open all worked, including Stop
halting a blind already in motion. The blind accepted **random rolling codes**,
so the firmware doesn't need to track the remote's sequence.

## The upstream RFPowerView bug

### Main bug: the CRC only works on the author's network

In `src/PacketCRC.cpp` (v0.0.3):

```cpp
PacketCRC::PacketCRC() : crc(CRC16(0x755B, 0xF48B, 0x0000, false, false)) {}
...
crc.add(packet, length + 2);   // checksum over the packet only
```

It computes the CRC over the packet only, starting from a fixed `0xF48B`. The
real protocol computes it over the **2-byte radio address followed by the
packet**, starting from `0xFFFF`:

```
CRC-16, poly 0x755B, init 0xFFFF, over [address high, address low] + packet
```

`0xF48B` is exactly the CRC state after feeding in `2E C8`, the address from
the author's own project, starting from `0xFFFF`. So the author's address is
effectively baked into the library. On any other network:

- **Receiving:** every real packet fails the check and is thrown away, so you
  hear nothing.
- **Sending:** every packet goes out with a wrong CRC, so the blind ignores it.

For our address, `0x0817`, the correct value is `0x0823`.

Nobody had noticed because the author tested on his own network, where the
shortcut gives the right answer. The address is also configurable everywhere
else in the library, so it looks as though it should work on any network.

**Our fix** (`lib/RFPowerView`): `PacketCRC` calculates its starting value
from the address passed to the `RFPowerView` constructor. It defaults to the
author's address, so his setup behaves exactly as before.

### Smaller issues

1. **A dependency that no longer installs.** The library asks for
   `robtillaart/CRC` version `1.0.2`, which is no longer in the PlatformIO
   registry, so PlatformIO quietly skips it and the build fails with
   `CRC16.h: No such file or directory`. Our copy computes the CRC itself and
   doesn't need that library.
2. **SPI inside an interrupt handler.** The library reads the radio over SPI
   from its IRQ handler. That's fine on a bench, but on an ESP8266 also
   running Wi-Fi it's a known source of occasional crashes (interrupt code
   needs to run from IRAM; the SPI and RF24 code doesn't). Our copy adds an
   optional `RFPOWERVIEW_POLL_IRQ` mode that reads the radio from `loop()`
   instead, which the firmware uses.

## In one sentence

We confirmed the channel by measuring signal strength, found the address by
trying all 256 possibilities for its first byte until real packets appeared,
and solved the checksum by noticing that it includes the address. That last
part is why RFPowerView as published only worked for its author.

## "Wait, you published your blind's radio ID?"

Yes. The address, remote ID and group for my kitchen blind are right there in
this repo, and with them and a $2 radio you could absolutely open and close
it. Congratulations.

You would, however, need to be within radio range, which in practice means
climbing over my fence into my backyard. I like to think I'd notice. And if
you're already in my backyard, operating the kitchen blind is not the most
alarming thing going on.

(Also, PowerView Gen 2 radio isn't encrypted, so anyone close enough could
capture the same IDs in a few minutes with the tools in this repo anyway.
Publishing them just saves you waiting for someone to press a button.)

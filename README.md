# kitchen-blinds-controller

A Wi-Fi bridge for a Hunter Douglas **PowerView Gen 2** blind, built from a
Wemos D1 Mini (ESP8266) and an nRF24L01+ radio. It sends the same 2.4 GHz
commands as the PowerView "Pebble" remote, so no PowerView Hub is needed, and
it exposes the blind to **Home Assistant** over MQTT.

- **Open / Close / Stop / Favorite** (move to saved position)
- **Home Assistant** via MQTT: a cover entity, a Favorite button and a
  position sensor, with retained state and availability
- **Stays in sync with the real remote:** it hears Pebble button presses and
  updates its state
- **Web UI** at `http://kitchen-blind.local/` for phones, plus a JSON `/state`
  endpoint
- **mDNS** and password-protected **OTA** updates

## Hardware

- Wemos D1 Mini (ESP8266)
- nRF24L01+ module (the common 8-pin board with a PCB antenna)
- A ~10 µF capacitor across the radio's VCC and GND (strongly recommended:
  the radio's transmit current spikes otherwise cause flaky behaviour)
- Any 5 V USB power supply

### Wiring

| nRF24L01+ pin | D1 Mini | GPIO | |
|---|---|---|---|
| 1 GND (square pad) | G | | |
| 2 VCC | **3V3** | | **3.3 V only.** 5 V destroys the radio |
| 3 CE | D1 | 5 | |
| 4 CSN | D8 | 15 | SPI chip select |
| 5 SCK | D5 | 14 | SPI clock |
| 6 MOSI | D7 | 13 | |
| 7 MISO | D6 | 12 | |
| 8 IRQ | D2 | 4 | Required for receiving |

The pins match the examples in the RFPowerView library. They're set as build
flags in `platformio.ini`.

## Setup

Requires [PlatformIO](https://platformio.org/).

1. **Credentials.** Copy the example files and fill them in (the real ones are
   git-ignored):

   ```sh
   cp include/secrets.example.h include/secrets.h   # Wi-Fi, MQTT, OTA password
   cp secrets.example.ini secrets.ini               # OTA password again, for uploads
   ```

2. **Your blind's IDs.** The radio address (`RF_ID`), remote ID (`REMOTE_ID`)
   and group (`BLIND_GROUP`) are set in `platformio.ini` under
   `[firmware_common]`. The values there are for my blind. To find yours
   without a hub, see [Finding your IDs](#finding-your-ids) below.

3. **Flash over USB** the first time:

   ```sh
   pio run -e firmware -t upload
   pio device monitor   # watch it join Wi-Fi and MQTT
   ```

4. **Later updates over Wi-Fi:**

   ```sh
   pio run -e firmware_ota -t upload
   ```

5. **Home Assistant:** add [`docs/home-assistant.yaml`](docs/home-assistant.yaml)
   to `configuration.yaml`, then reload "Manually configured MQTT entities".
   [`docs/home-assistant-automations.yaml`](docs/home-assistant-automations.yaml)
   has example automations: open at sunrise, and move to Favorite before the
   low afternoon sun starts glaring in.

## MQTT topics

| Topic | Direction | Payload |
|---|---|---|
| `home/kitchen_blind/set` | to device | `OPEN`, `CLOSE`, `STOP`, `FAVORITE` |
| `home/kitchen_blind/state` | from device, retained | `open`, `closed`, `stopped` |
| `home/kitchen_blind/attributes` | from device, retained | `{"last_command": "...", "source": "mqtt\|web\|remote"}` |
| `home/kitchen_blind/availability` | from device, retained (LWT) | `online`, `offline` |

The blind doesn't report its position, so the state is the last command sent,
from any source. Favorite reports as `closed` because, for this blind, the
favorite position is "as closed as we want it". Change the mapping in
`COMMANDS` in `src/firmware.cpp` if yours is different. After a reboot, the
device restores its state from its own retained messages.

## Finding your IDs

PowerView radio isn't encrypted, but RFPowerView expects you to read the
radio address from a PowerView Hub. These bench tools find it with only a
Pebble remote:

| Environment | What it does |
|---|---|
| `smoketest` | Checks SPI wiring to the radio and prints its registers |
| `scanner` | Sweeps all 126 channels for RF energy, to confirm the remote is on channel 7 |
| `findid` | **Finds the radio address.** Tries all 256 values of the address's first byte, 6 at a time, and reports frames that decode as PowerView packets |
| `listen` | Decodes button presses on a known address: remote ID, group, command |
| `txtest` | Sends Open/Stop/Favorite from serial keystrokes (`o`, `s`, `f`) |
| `discover`, `discover_raw` | Earlier "promiscuous mode" sniffer attempts; kept for reference |

Typical flow: flash `findid` and press Stop on the remote a few times, near
the radio. It prints `RF_ID=0x....` with a "CRC failed" note, which is
expected because it checks with RFPowerView's original CRC. Then put that
address in the `listen` environment, flash it, and press buttons to get the
remote ID and group.

## Protocol notes and the RFPowerView fix

[`docs/protocol-notes.md`](docs/protocol-notes.md) has the full write-up:
packet layout, captured examples, rolling codes, and the CRC.
[`docs/how-we-found-it.md`](docs/how-we-found-it.md) tells the story of how
the protocol and the library bug were worked out, step by step.

The short version: the packet CRC is CRC-16 (poly `0x755B`, init `0xFFFF`)
over the **2-byte radio address followed by the packet**. Upstream
RFPowerView v0.0.3 hard-codes the CRC state for its author's address, so it
can't receive or send on any other network. `lib/RFPowerView` is a copy with
that fixed, plus an optional mode that reads the radio from `loop()` instead
of an interrupt, which is more reliable on the ESP8266 with Wi-Fi active. See
[`lib/RFPowerView/README.local.md`](lib/RFPowerView/README.local.md).

## Credits

- [RFPowerView](https://github.com/mattyway/RFPowerView) and
  [PowerViewSniffer](https://github.com/mattyway/PowerViewSniffer) by Matt Way,
  which reverse-engineered the PowerView packet format
- [RF24](https://github.com/nRF24/RF24) and
  [PubSubClient](https://github.com/knolleary/pubsubclient)
- Travis Goodspeed's [nRF24 promiscuous-mode technique](https://travisgoodspeed.blogspot.com/2011/02/promiscuity-is-nrf24l01s-duty.html), which inspired the
  discovery tools

## License

MIT (see [LICENSE](LICENSE)), except `lib/RFPowerView/`, which remains under
GPL-2.0-only. Because the firmware links that library, any compiled firmware
you distribute falls under the GPL.

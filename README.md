# esp32-radio-remote-for-microbit

An ESP32 (ESPHome) remote control for the micro:bit that speaks the micro:bit radio protocol.

The ESP32 sends packets in the micro:bit radio format, and the micro:bit receives them with
the built-in **Radio** blocks in MakeCode. No pairing, MAC address or extension is needed.
The micro:bit frame is hidden inside the data of an ordinary BLE advertisement ("packet-in-packet")
sent on a single advertising channel. The component drives the BLE controller directly
through VHCI, without the Bluedroid/NimBLE host stack.

The only extra step for students: besides `radio.setGroup()`, the program needs the
`radio.setFrequencyBand(2)` block (Radio → more).

![The remote](docs/img/remote-front.jpg)

## Hardware

A WEMOS LOLIN32 Lite, a KY-023 joystick, four 12×12 mm buttons and two AA batteries on a perfboard.
Photos, parts list, schematic and wiring table: [docs/build.md](docs/build.md).

![Wiring schematic](docs/schematic.svg)

## Files

| File | What it is |
|---|---|
| `components/microbit_radio/` | ESPHome external component |
| `microbit-radio-remote.yaml` | test firmware (same pins as `bt-microbit-remote.yaml` in [pxe-diy-bt-remote](https://github.com/sargo/pxe-diy-bt-remote)) |
| `docs/` | building the remote: photos, parts list, schematic |
| `makecode-test.ts` | micro:bit test program (built-in blocks only) |
| `microbit-radio-encoder.mjs` | reference encoder and simulated nRF receiver (`node microbit-radio-encoder.mjs`) |
| `test-frame-header.mjs` | checks the logic of `microbit_frame.h` without a C++ compiler (`node test-frame-header.mjs`) |
| `gen-vectors.mjs` | generates the self-test vectors in `microbit_frame.h` |

## Using it in ESPHome

ESPHome fetches the component straight from this repository, so there is nothing to copy:

```yaml
external_components:
  - source: github://sargo/esp32-radio-remote-for-microbit@main
    components: [microbit_radio]
    refresh: 0s     # while testing: always fetch the latest version

microbit_radio:
  id: mb_radio
  group: 1          # radio.setGroup(1)
  ble_channel: 37   # 37 -> band 2, 38 -> band 26, 39 -> band 80
  interval: 100ms   # BLE advertising interval
  repeat: 1         # copies of each message the micro:bit receives
```

Sending from lambdas:

```cpp
id(mb_radio).send_number(42);        // radio.onReceivedNumber
id(mb_radio).send_value("A", 1);     // radio.onReceivedValue, name up to 5 characters
id(mb_radio).send_string("hello");   // radio.onReceivedString, up to 9 characters
id(mb_radio).set_group(3);           // applies to messages queued afterwards
```

## Step-by-step test

1. Flash the firmware: `esphome run microbit-radio-remote.yaml`.
   The log should show:
   - `Frame encoder self-test OK`
   - `Ready: group 1, micro:bit frequency band 2`
   - `Send number N` every 2 s
2. On the micro:bit, paste `makecode-test.ts` into the JavaScript tab at makecode.microbit.org and flash it.
3. Expected result:
   - every 2 s the micro:bit shows the next digit 0–9,
   - pressing a button on the remote shows a letter (A, B, X, Y, C, N, S, E, W), and releasing it clears the screen.
4. Packet loss: button A on the micro:bit shows how many digits arrived. Compare it with the number of `Send number` lines in the ESP32 log.

## If nothing arrives

- Check that the group and the band match on both sides.
- Try `ble_channel: 39` with `radio.setFrequencyBand(80)`. Channel 37 (2402 MHz) sits at the edge of Wi-Fi channel 1.
- Look for `status 0x..` errors from HCI commands in the ESP32 log.
- If only some packets arrive, set `repeat: 2` (the micro:bit then receives copies) or an `interval` longer than 100 ms.
- A second micro:bit sending `radio.sendNumber()` on the same group and band confirms that the receiver works.
- After the first build with this component, or after changing its source, use **Clean Build Files** in ESPHome.
  The IDF build caches its list of source files, and a stale list causes `undefined reference` link errors.

## Limitations and things not yet tested on hardware

- A packet has to fit into the 31-byte advertisement: integers (int32), value names up to 5 characters, strings up to 9 characters.
- One direction only (ESP32 → micro:bit).
- Do not combine it with `esp32_ble`, `esp32_ble_tracker`, `ble_client` or `bluetooth_proxy`: they use the same controller.
- Running Wi-Fi alongside this component has not been tested.
- Not verifiable without hardware:
  - the CRC bit order of the Nordic radio (taken from Nordic's documentation),
  - frequency deviation tolerance (BLE ±250 kHz, micro:bit about ±170 kHz),
  - the delay from enabling advertising to the first packet (the transmit window is half of `interval`).

## Sources

- Frame format: [codal-microbit-v2 `MicroBitRadio.cpp`](https://github.com/lancaster-university/codal-microbit-v2/blob/master/source/MicroBitRadio.cpp)
  and [pxt-common-packages `radio.ts`](https://github.com/microsoft/pxt-common-packages/blob/master/libs/radio/radio.ts).
- The simulated receiver in `microbit-radio-encoder.mjs` uses a whitening function ported from the
  ESPeranto library ([woot2023_espwn32_artifacts](https://github.com/RCayre/woot2023_espwn32_artifacts),
  MIT license, © 2023 Romain Cayre, Damien Cauquil, Aurélien Francillon).

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
| `microbit-radio-remote.yaml` | ESPHome configuration of the remote |
| `makecode-example.ts` | example micro:bit program (built-in blocks only) |
| `docs/` | building the remote: photos, parts list, schematic |

Development tools, needed only when changing the frame encoder in `microbit_frame.h`:

| File | What it is |
|---|---|
| `tools/microbit-radio-encoder.mjs` | reference encoder and simulated nRF receiver (`node tools/microbit-radio-encoder.mjs`) |
| `tools/test-frame-header.mjs` | checks the logic of `microbit_frame.h` without a C++ compiler (`node tools/test-frame-header.mjs`) |
| `tools/gen-vectors.mjs` | generates the self-test vectors in `microbit_frame.h` (`node tools/gen-vectors.mjs`) |

## Using it in ESPHome

ESPHome fetches the component straight from this repository, so there is nothing to copy:

```yaml
external_components:
  - source: github://sargo/esp32-radio-remote-for-microbit@main
    components: [microbit_radio]

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

## Getting started

1. Flash the firmware: `esphome run microbit-radio-remote.yaml`.
   The log shows `Frame encoder self-test OK` and `Ready: group 1, micro:bit frequency band 2`,
   and the onboard LED turns on once the radio is ready.
2. On the micro:bit, paste `makecode-example.ts` into the JavaScript tab at makecode.microbit.org and flash it.
3. Pressing a button on the remote shows a letter on the micro:bit (A, B, X, Y, C, N, S, E, W), and releasing it clears the screen.

## Troubleshooting

- **Nothing arrives:** check that the group and the band match on both sides.
  Try `ble_channel: 39` with `radio.setFrequencyBand(80)`: channel 37 (2402 MHz) sits at the edge of Wi-Fi channel 1.
- **HCI errors:** look for `status 0x..` messages from the component in the ESP32 log.
- **Some messages get lost:** set `repeat: 2` (the micro:bit then receives copies) or an `interval` longer than 100 ms.
- **Slow start on batteries:** two AA cells sag during RF calibration at boot. The configuration lowers the CPU
  clock and enables `CONFIG_ESP_PHY_REDUCE_TX_POWER`, which brings the start-up time down to about 3 s.
  See [Power](docs/build.md#power).
- **`undefined reference` link errors:** use **Clean Build Files** in ESPHome after adding the component or
  changing its source. The IDF build caches its list of source files.

## Limitations

- A packet has to fit into the 31-byte advertisement: integers (int32), value names up to 5 characters, strings up to 9 characters.
- One direction only (ESP32 → micro:bit).
- Do not combine it with `esp32_ble`, `esp32_ble_tracker`, `ble_client` or `bluetooth_proxy`: they use the same controller.
- Running Wi-Fi alongside this component has not been tested.

## Sources

- Frame format: [codal-microbit-v2 `MicroBitRadio.cpp`](https://github.com/lancaster-university/codal-microbit-v2/blob/master/source/MicroBitRadio.cpp)
  and [pxt-common-packages `radio.ts`](https://github.com/microsoft/pxt-common-packages/blob/master/libs/radio/radio.ts).
- The simulated receiver in `tools/microbit-radio-encoder.mjs` uses a whitening function ported from the
  ESPeranto library ([woot2023_espwn32_artifacts](https://github.com/RCayre/woot2023_espwn32_artifacts),
  MIT license, © 2023 Romain Cayre, Damien Cauquil, Aurélien Francillon).

// micro:bit program for the ESP32 remote (microbit-radio-remote.yaml).
// Built-in blocks only: paste into the JavaScript tab of makecode.microbit.org
// or build the same with blocks ("set frequency band" is under Radio -> more).

radio.setGroup(1)
radio.setFrequencyBand(2) // ESP32 ble_channel 37
basic.showIcon(IconNames.Heart)

// name: "A", "B", "X", "Y" (buttons), "C" (joystick click),
//       "N", "S", "E", "W" (joystick directions); value: 1 = pressed, 0 = released
radio.onReceivedValue(function (name, value) {
    if (value == 1) {
        basic.showString(name)
    } else {
        basic.clearScreen()
    }
})

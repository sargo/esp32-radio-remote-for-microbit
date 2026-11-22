// micro:bit test program for microbit-radio-remote.yaml.
// Only built-in blocks: paste into the JavaScript tab of makecode.microbit.org
// (or build the same with blocks; "set frequency band" is under Radio -> more).

let received = 0

radio.setGroup(1)
radio.setFrequencyBand(2) // ESP32 ble_channel 37
basic.showIcon(IconNames.Heart)

// Heartbeat 0..9 sent by the ESP32 every 2 s
radio.onReceivedNumber(function (receivedNumber) {
    received += 1
    basic.showNumber(receivedNumber)
    serial.writeValue("rssi", radio.receivedPacket(RadioPacketProperty.SignalStrength))
})

// Buttons and joystick: name = "A", "B", "X", "Y", "C", "N", "S", "E", "W"
radio.onReceivedValue(function (name, value) {
    if (value == 1) {
        basic.showString(name)
    } else {
        basic.clearScreen()
    }
})

radio.onReceivedString(function (receivedString) {
    basic.showString(receivedString)
})

// Button A on the micro:bit: how many heartbeats arrived (compare with the ESP32 log)
input.onButtonPressed(Button.A, function () {
    basic.showNumber(received)
})

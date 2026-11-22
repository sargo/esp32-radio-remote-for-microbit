"""Send micro:bit (MakeCode "radio") packets from an ESP32 via BLE advertising.

The micro:bit receives them with the built-in radio blocks after
radio.setGroup(<group>) and radio.setFrequencyBand(<band>), where band is
2 / 26 / 80 for BLE channel 37 / 38 / 39.

Use from lambdas:
  id(mb_radio).send_number(42);
  id(mb_radio).send_value("A", 1);      // name up to 5 characters
  id(mb_radio).send_string("hello");    // up to 9 characters
  id(mb_radio).set_group(3);            // applies to messages queued later
"""

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import add_idf_sdkconfig_option, get_esp32_variant
from esphome.components.esp32.const import VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import CONF_GROUP, CONF_ID, CONF_INTERVAL, CONF_REPEAT

CODEOWNERS = []
DEPENDENCIES = ["esp32"]
# The component drives the BLE controller directly over VHCI, without a host stack.
CONFLICTS_WITH = [
    "esp32_ble",
    "esp32_ble_tracker",
    "esp32_ble_beacon",
    "esp32_ble_server",
    "ble_client",
    "bluetooth_proxy",
]

CONF_BLE_CHANNEL = "ble_channel"
CONF_SERIAL_NUMBER = "serial_number"

microbit_radio_ns = cg.esphome_ns.namespace("microbit_radio")
MicrobitRadio = microbit_radio_ns.class_("MicrobitRadio", cg.Component)


def _interval(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 20 <= value.total_milliseconds <= 1000:
        raise cv.Invalid("interval must be between 20ms and 1000ms")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MicrobitRadio),
        # micro:bit radio.setGroup()
        cv.Optional(CONF_GROUP, default=0): cv.int_range(min=0, max=255),
        # 37 -> band 2 (2402 MHz), 38 -> band 26 (2426 MHz), 39 -> band 80 (2480 MHz)
        cv.Optional(CONF_BLE_CHANNEL, default=37): cv.one_of(37, 38, 39, int=True),
        # BLE advertising interval; the controller may require >= 100ms
        cv.Optional(CONF_INTERVAL, default="100ms"): _interval,
        # advertising events (= copies received by the micro:bit) per message
        cv.Optional(CONF_REPEAT, default=1): cv.int_range(min=1, max=10),
        # value of MakeCode's "received packet serial number"
        cv.Optional(CONF_SERIAL_NUMBER, default=0): cv.uint32_t,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_group(config[CONF_GROUP]))
    cg.add(var.set_ble_channel(config[CONF_BLE_CHANNEL]))
    cg.add(var.set_interval(int(config[CONF_INTERVAL].total_milliseconds)))
    cg.add(var.set_repeat(config[CONF_REPEAT]))
    cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))

    # Newer ESPHome excludes the IDF "bt" component unless it is requested.
    if hasattr(esp32, "request_bluetooth"):
        esp32.request_bluetooth()

    # Bluetooth controller only (no Bluedroid / NimBLE), reached through VHCI.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", False)
    if get_esp32_variant() == VARIANT_ESP32:
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", True)
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", False)
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)

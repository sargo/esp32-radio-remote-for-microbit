#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <array>
#include <cstdint>
#include <deque>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "microbit_frame.h"

namespace esphome {
namespace microbit_radio {

/// Sends MakeCode "radio" packets (number / name+value / string) that a stock
/// micro:bit receives with the built-in radio blocks. The frames ride inside
/// legacy BLE advertisements sent through the controller's VHCI interface, so no
/// BLE host stack, pairing or MAC address is involved.
///
/// micro:bit side: radio.setGroup(<group>) and radio.setFrequencyBand(<band>),
/// band = 2 / 26 / 80 for BLE channel 37 / 38 / 39.
class MicrobitRadio : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  void set_group(uint8_t group) { this->group_ = group; }
  uint8_t get_group() const { return this->group_; }
  void set_ble_channel(uint8_t channel) { this->ble_channel_ = channel; }
  void set_interval(uint32_t interval_ms) { this->interval_ms_ = interval_ms; }
  void set_repeat(uint8_t repeat) { this->repeat_ = repeat; }
  void set_serial_number(uint32_t serial) { this->serial_ = serial; }

  /// micro:bit: radio.onReceivedNumber
  bool send_number(int32_t value);
  /// micro:bit: radio.onReceivedValue (name up to 5 characters)
  bool send_value(const std::string &name, int32_t value);
  /// micro:bit: radio.onReceivedString (up to 9 characters)
  bool send_string(const std::string &text);

  /// True once the controller accepted the advertising parameters.
  bool is_ready() const;
  /// micro:bit radio.setFrequencyBand() value matching the configured BLE channel.
  uint8_t get_microbit_band() const;
  uint32_t get_sent_count() const { return this->sent_; }

 protected:
  enum class State : uint8_t {
    RESET,         // HCI_Reset
    SET_PARAMS,    // LE Set Advertising Parameters
    IDLE,          // waiting for a queued frame
    SET_DATA,      // LE Set Advertising Data
    ENABLE,        // LE Set Advertising Enable (1)
    ADVERTISING,   // controller is sending advertising events
    DISABLE,       // LE Set Advertising Enable (0)
    FAILED,        // retry from RESET after a pause
  };
  enum class CmdResult : uint8_t { PENDING, OK, ERROR };

  bool enqueue_(const uint8_t *packet, size_t len, const char *what);
  CmdResult run_command_(uint16_t opcode, const uint8_t *params, uint8_t len, uint8_t *status);
  void set_state_(State state);
  uint32_t advertising_window_ms_() const;

  std::deque<std::array<uint8_t, frame::ADV_DATA_LEN>> queue_;
  std::array<uint8_t, frame::ADV_DATA_LEN> current_{};
  HighFrequencyLoopRequester high_freq_;

  State state_{State::RESET};
  uint32_t state_since_{0};
  bool cmd_in_flight_{false};
  uint32_t cmd_sent_at_{0};

  uint8_t group_{0};
  uint8_t ble_channel_{37};
  uint8_t repeat_{1};
  uint32_t interval_ms_{100};
  uint32_t serial_{0};
  uint32_t sent_{0};
  uint32_t dropped_{0};
};

}  // namespace microbit_radio
}  // namespace esphome

#endif  // USE_ESP32

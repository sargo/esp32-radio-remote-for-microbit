#include "microbit_radio.h"

#ifdef USE_ESP32

#include <atomic>
#include <cstring>

#include "esp_bt.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifndef CONFIG_BT_ENABLED
#error "microbit_radio needs CONFIG_BT_ENABLED (set automatically by the component's __init__.py)"
#endif

#ifdef USE_ARDUINO
// Stop Arduino from releasing BT memory at startup (same trick as ESPHome's esp32_ble).
extern "C" bool btInUse() { return true; }  // NOLINT(readability-identifier-naming)
#endif

namespace esphome {
namespace microbit_radio {

static const char *const TAG = "microbit_radio";

static constexpr uint8_t HCI_COMMAND_PKT = 0x01;
static constexpr uint8_t HCI_EVENT_PKT = 0x04;
static constexpr uint8_t HCI_EVT_COMMAND_COMPLETE = 0x0E;
static constexpr uint8_t HCI_EVT_COMMAND_STATUS = 0x0F;
static constexpr uint16_t HCI_RESET = 0x0C03;
static constexpr uint16_t HCI_LE_SET_ADV_PARAMS = 0x2006;
static constexpr uint16_t HCI_LE_SET_ADV_DATA = 0x2008;
static constexpr uint16_t HCI_LE_SET_ADV_ENABLE = 0x200A;
static constexpr uint8_t ADV_NONCONN_IND = 0x03;
static constexpr uint8_t HCI_ERR_INVALID_PARAMS = 0x12;

static constexpr uint32_t CMD_TIMEOUT_MS = 1000;
static constexpr uint32_t RETRY_AFTER_FAIL_MS = 5000;
static constexpr size_t MAX_QUEUE = 16;

// Written from the BT controller task (VHCI callback), read from the main loop.
static std::atomic<bool> g_cmd_done{false};
static std::atomic<uint16_t> g_cmd_opcode{0};
static std::atomic<uint8_t> g_cmd_status{0};

static void on_controller_ready() {}

static int on_hci_packet(uint8_t *data, uint16_t len) {
  // Command Complete: 04 0E plen ncmd op_lo op_hi status ...
  if (len >= 7 && data[0] == HCI_EVENT_PKT && data[1] == HCI_EVT_COMMAND_COMPLETE) {
    g_cmd_opcode.store(static_cast<uint16_t>(data[4] | (data[5] << 8)));
    g_cmd_status.store(data[6]);
    g_cmd_done.store(true);
  }
  // Command Status: 04 0F plen status ncmd op_lo op_hi
  else if (len >= 7 && data[0] == HCI_EVENT_PKT && data[1] == HCI_EVT_COMMAND_STATUS) {
    g_cmd_opcode.store(static_cast<uint16_t>(data[5] | (data[6] << 8)));
    g_cmd_status.store(data[3]);
    g_cmd_done.store(true);
  }
  return 0;
}

static const esp_vhci_host_callback_t VHCI_CALLBACKS = {on_controller_ready, on_hci_packet};

void MicrobitRadio::setup() {
  const int failed_vector = frame::self_test();
  if (failed_vector != 0) {
    ESP_LOGE(TAG, "Frame encoder self-test FAILED (vector %d)", failed_vector);
    this->mark_failed();
    return;
  }
  ESP_LOGD(TAG, "Frame encoder self-test OK");

  esp_err_t err;
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_BTDM_CTRL_MODE_BLE_ONLY)
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
    err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_BTDM_CTRL_MODE_BTDM)
    const esp_bt_mode_t mode = ESP_BT_MODE_BTDM;
#else
    const esp_bt_mode_t mode = ESP_BT_MODE_BLE;
#endif
    err = esp_bt_controller_enable(mode);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }
  err = esp_vhci_host_register_callback(&VHCI_CALLBACKS);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vhci_host_register_callback failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->set_state_(State::RESET);
}

void MicrobitRadio::loop() {
  uint8_t status = 0;
  CmdResult res;

  switch (this->state_) {
    case State::RESET:
      res = this->run_command_(HCI_RESET, nullptr, 0, &status);
      if (res == CmdResult::OK) {
        this->set_state_(State::SET_PARAMS);
      } else if (res == CmdResult::ERROR) {
        ESP_LOGE(TAG, "HCI reset failed (status 0x%02X)", status);
        this->set_state_(State::FAILED);
      }
      break;

    case State::SET_PARAMS: {
      const uint16_t units = static_cast<uint16_t>(this->interval_ms_ * 1000 / 625);  // 0.625 ms units
      const uint8_t channel_map = static_cast<uint8_t>(1u << (this->ble_channel_ - 37));
      const uint8_t params[15] = {
          static_cast<uint8_t>(units & 0xFF), static_cast<uint8_t>(units >> 8),  // interval min
          static_cast<uint8_t>(units & 0xFF), static_cast<uint8_t>(units >> 8),  // interval max
          ADV_NONCONN_IND,                                                         // type
          0x00,                                                                    // own address: public
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                // peer (unused)
          channel_map,                                                             // single channel
          0x00,                                                                    // filter policy
      };
      res = this->run_command_(HCI_LE_SET_ADV_PARAMS, params, sizeof(params), &status);
      if (res == CmdResult::OK) {
        ESP_LOGI(TAG, "Ready: group %u, micro:bit frequency band %u", this->group_, this->get_microbit_band());
        this->set_state_(State::IDLE);
      } else if (res == CmdResult::ERROR) {
        if (status == HCI_ERR_INVALID_PARAMS && this->interval_ms_ < 100) {
          ESP_LOGW(TAG, "Controller rejected %u ms interval, falling back to 100 ms",
                   static_cast<unsigned>(this->interval_ms_));
          this->interval_ms_ = 100;  // retried on the next loop
        } else {
          ESP_LOGE(TAG, "LE Set Advertising Parameters failed (status 0x%02X)", status);
          this->set_state_(State::FAILED);
        }
      }
      break;
    }

    case State::IDLE:
      if (!this->queue_.empty()) {
        this->current_ = this->queue_.front();
        this->queue_.pop_front();
        this->set_state_(State::SET_DATA);
      } else {
        this->high_freq_.stop();
      }
      break;

    case State::SET_DATA: {
      uint8_t params[1 + frame::ADV_DATA_LEN];
      params[0] = frame::ADV_DATA_LEN;
      memcpy(params + 1, this->current_.data(), frame::ADV_DATA_LEN);
      res = this->run_command_(HCI_LE_SET_ADV_DATA, params, sizeof(params), &status);
      if (res == CmdResult::OK) {
        this->set_state_(State::ENABLE);
      } else if (res == CmdResult::ERROR) {
        ESP_LOGW(TAG, "LE Set Advertising Data failed (status 0x%02X), message dropped", status);
        this->dropped_++;
        this->set_state_(State::IDLE);
      }
      break;
    }

    case State::ENABLE: {
      const uint8_t enable = 1;
      res = this->run_command_(HCI_LE_SET_ADV_ENABLE, &enable, 1, &status);
      if (res == CmdResult::OK) {
        this->set_state_(State::ADVERTISING);
      } else if (res == CmdResult::ERROR) {
        ESP_LOGW(TAG, "LE Set Advertising Enable failed (status 0x%02X), message dropped", status);
        this->dropped_++;
        this->set_state_(State::IDLE);
      }
      break;
    }

    case State::ADVERTISING:
      if (millis() - this->state_since_ >= this->advertising_window_ms_())
        this->set_state_(State::DISABLE);
      break;

    case State::DISABLE: {
      const uint8_t enable = 0;
      res = this->run_command_(HCI_LE_SET_ADV_ENABLE, &enable, 1, &status);
      if (res == CmdResult::OK) {
        this->sent_++;
        ESP_LOGV(TAG, "Message #%u sent", static_cast<unsigned>(this->sent_));
        this->set_state_(State::IDLE);
      } else if (res == CmdResult::ERROR) {
        ESP_LOGW(TAG, "Disabling advertising failed (status 0x%02X), resetting controller", status);
        this->set_state_(State::RESET);
      }
      break;
    }

    case State::FAILED:
      if (millis() - this->state_since_ >= RETRY_AFTER_FAIL_MS)
        this->set_state_(State::RESET);
      break;
  }
}

MicrobitRadio::CmdResult MicrobitRadio::run_command_(uint16_t opcode, const uint8_t *params, uint8_t len,
                                                     uint8_t *status) {
  if (!this->cmd_in_flight_) {
    if (!esp_vhci_host_check_send_available())
      return CmdResult::PENDING;  // controller busy, try again on the next loop
    uint8_t buf[4 + 1 + frame::ADV_DATA_LEN];
    buf[0] = HCI_COMMAND_PKT;
    buf[1] = static_cast<uint8_t>(opcode & 0xFF);
    buf[2] = static_cast<uint8_t>(opcode >> 8);
    buf[3] = len;
    if (len > 0)
      memcpy(buf + 4, params, len);
    g_cmd_done.store(false);
    esp_vhci_host_send_packet(buf, static_cast<uint16_t>(4 + len));
    this->cmd_in_flight_ = true;
    this->cmd_sent_at_ = millis();
    return CmdResult::PENDING;
  }
  if (g_cmd_done.load() && g_cmd_opcode.load() == opcode) {
    this->cmd_in_flight_ = false;
    *status = g_cmd_status.load();
    return *status == 0 ? CmdResult::OK : CmdResult::ERROR;
  }
  if (millis() - this->cmd_sent_at_ > CMD_TIMEOUT_MS) {
    this->cmd_in_flight_ = false;
    *status = 0xFF;  // timeout
    return CmdResult::ERROR;
  }
  return CmdResult::PENDING;
}

void MicrobitRadio::set_state_(State state) {
  this->state_ = state;
  this->state_since_ = millis();
  if (state == State::FAILED) {
    this->high_freq_.stop();
  } else if (state != State::IDLE) {
    this->high_freq_.start();  // precise timing of the advertising window
  }
}

// Long enough for `repeat` advertising events, short enough to never start one more.
// Events come every interval + 0..10 ms (BLE advDelay).
uint32_t MicrobitRadio::advertising_window_ms_() const {
  return (this->repeat_ - 1) * (this->interval_ms_ + 10) + this->interval_ms_ / 2;
}

bool MicrobitRadio::enqueue_(const uint8_t *packet, size_t len, const char *what) {
  if (this->is_failed())
    return false;
  std::array<uint8_t, frame::ADV_DATA_LEN> adv;
  if (len == 0 || !frame::build_adv_data(this->group_, packet, len, this->ble_channel_, adv.data())) {
    ESP_LOGW(TAG, "%s does not fit into one micro:bit radio packet", what);
    return false;
  }
  if (this->queue_.size() >= MAX_QUEUE) {
    this->queue_.pop_front();
    this->dropped_++;
    ESP_LOGW(TAG, "Queue full, oldest message dropped");
  }
  this->queue_.push_back(adv);
  this->high_freq_.start();
  return true;
}

bool MicrobitRadio::send_number(int32_t value) {
  uint8_t pkt[frame::MAX_MAKECODE_LEN];
  const size_t n = frame::make_number(pkt, value, millis(), this->serial_);
  ESP_LOGD(TAG, "Send number %d (group %u)", static_cast<int>(value), this->group_);
  return this->enqueue_(pkt, n, "Number");
}

bool MicrobitRadio::send_value(const std::string &name, int32_t value) {
  uint8_t pkt[frame::MAX_MAKECODE_LEN];
  const size_t n = frame::make_value(pkt, name.data(), name.size(), value, millis(), this->serial_);
  if (n == 0) {
    ESP_LOGW(TAG, "Value name '%s' is too long (max %u characters)", name.c_str(),
             static_cast<unsigned>(frame::MAX_VALUE_NAME_LEN));
    return false;
  }
  ESP_LOGD(TAG, "Send value %s=%d (group %u)", name.c_str(), static_cast<int>(value), this->group_);
  return this->enqueue_(pkt, n, "Value");
}

bool MicrobitRadio::send_string(const std::string &text) {
  uint8_t pkt[frame::MAX_MAKECODE_LEN];
  const size_t n = frame::make_string(pkt, text.data(), text.size(), millis(), this->serial_);
  if (n == 0) {
    ESP_LOGW(TAG, "String '%s' is too long (max %u characters)", text.c_str(),
             static_cast<unsigned>(frame::MAX_STRING_LEN));
    return false;
  }
  ESP_LOGD(TAG, "Send string \"%s\" (group %u)", text.c_str(), this->group_);
  return this->enqueue_(pkt, n, "String");
}

bool MicrobitRadio::is_ready() const {
  switch (this->state_) {
    case State::IDLE:
    case State::SET_DATA:
    case State::ENABLE:
    case State::ADVERTISING:
    case State::DISABLE:
      return true;
    default:
      return false;
  }
}

uint8_t MicrobitRadio::get_microbit_band() const {
  switch (this->ble_channel_) {
    case 38:
      return 26;  // 2426 MHz
    case 39:
      return 80;  // 2480 MHz
    default:
      return 2;  // channel 37, 2402 MHz
  }
}

void MicrobitRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "micro:bit radio over BLE advertising:");
  ESP_LOGCONFIG(TAG, "  Group: %u", this->group_);
  ESP_LOGCONFIG(TAG, "  BLE channel: %u -> micro:bit radio.setFrequencyBand(%u)", this->ble_channel_,
                this->get_microbit_band());
  ESP_LOGCONFIG(TAG, "  Interval: %u ms, repeat: %u, window: %u ms", static_cast<unsigned>(this->interval_ms_),
                this->repeat_, static_cast<unsigned>(this->advertising_window_ms_()));
  ESP_LOGCONFIG(TAG, "  Serial number field: %u", static_cast<unsigned>(this->serial_));
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup failed, see the log above");
}

}  // namespace microbit_radio
}  // namespace esphome

#endif  // USE_ESP32

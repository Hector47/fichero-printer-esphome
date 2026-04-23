#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/automation.h"
#include "esphome/components/ble_client/ble_client.h"

#include <vector>
#include <cstdint>

namespace esphome {
namespace fichero_printer {

// ---------------------------------------------------------------------------
// BLE service / characteristic identifiers (18f0 UART service)
//   Service  : 000018f0-0000-1000-8000-00805f9b34fb  (short: 0x18F0)
//   Write    : 00002af1-0000-1000-8000-00805f9b34fb  (short: 0x2AF1)
//   Notify   : 00002af0-0000-1000-8000-00805f9b34fb  (short: 0x2AF0)
// ---------------------------------------------------------------------------
static const uint16_t FICHERO_SERVICE_UUID = 0x18F0;
static const uint16_t WRITE_CHAR_UUID = 0x2AF1;
static const uint16_t NOTIFY_CHAR_UUID = 0x2AF0;

// Printhead geometry
static const uint8_t PRINTHEAD_PX = 96;
static const uint8_t BYTES_PER_ROW = PRINTHEAD_PX / 8;  // 12

// BLE chunk size (bytes per write_char call).  Keep under the negotiated MTU;
// 200 bytes matches the Python reference implementation.
static const uint16_t BLE_CHUNK_SIZE = 200;

// Timing delays (milliseconds) – empirically tuned against D11s fw 2.4.6
static const uint32_t DELAY_AFTER_DENSITY_MS = 100;
static const uint32_t DELAY_COMMAND_GAP_MS = 50;
static const uint32_t DELAY_RASTER_SETTLE_MS = 500;
static const uint32_t DELAY_AFTER_FEED_MS = 300;
static const uint32_t DELAY_CHUNK_GAP_MS = 20;
static const uint32_t PRINT_TIMEOUT_MS = 60000;

// ---------------------------------------------------------------------------
// Print-sequence state machine
// ---------------------------------------------------------------------------
enum class PrintState : uint8_t {
  IDLE = 0,
  SEND_DENSITY,    ///< 10 FF 10 00 nn  – set print density
  SEND_PAPER,      ///< 10 FF 84 nn     – set paper type
  SEND_WAKEUP,     ///< 00 * 12         – wake up the printhead
  SEND_ENABLE,     ///< 10 FF FE 01     – AiYin enable
  SEND_RASTER,     ///< GS v 0 header + raster data (chunked)
  SEND_FORM_FEED,  ///< 1D 0C           – advance to next label
  SEND_STOP,       ///< 10 FF FE 45     – AiYin stop; starts wait for ACK
  WAIT_STOP,       ///< waiting for 0xAA / "OK" notification or timeout
};

// ---------------------------------------------------------------------------
// FicheroPrinter component
// ---------------------------------------------------------------------------
class FicheroPrinter : public Component, public ble_client::BLEClientNode {
 public:
  // ----- Configuration (set from __init__.py) -----
  void set_density(uint8_t density) { this->density_ = density; }
  void set_paper_type(uint8_t paper_type) { this->paper_type_ = paper_type; }

  // ----- ESPHome Component overrides -----
  void setup() override {}
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ----- BLEClientNode overrides -----
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param) override {}

  // ----- Print API -----
  /// Start printing pre-processed 1-bit raster data.
  /// @p data must be an exact multiple of BYTES_PER_ROW (12) bytes.
  /// Rows are deduced as data.size() / 12.
  void print_raw(const std::vector<uint8_t> &data);

 protected:
  void find_characteristics_();
  void write_bytes_(const uint8_t *data, size_t len);
  void advance_state_();
  void reset_print_();

  // Config
  uint8_t density_{1};
  uint8_t paper_type_{0};

  // GATT handles discovered after service search
  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};

  // State machine
  PrintState state_{PrintState::IDLE};
  uint32_t state_deadline_{0};

  // Raster payload
  std::vector<uint8_t> raster_data_;
  uint16_t raster_rows_{0};
  size_t raster_offset_{0};

  // Notification tracking (stop-command response)
  bool response_received_{false};
  uint8_t response_byte_{0};
};

// ---------------------------------------------------------------------------
// PrintAction – ESPHome automation action
// ---------------------------------------------------------------------------
template<typename... Ts>
class PrintAction : public Action<Ts...>, public Parented<FicheroPrinter> {
 public:
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

  void play(Ts... x) override {
    this->parent_->print_raw(this->data_.value(x...));
  }
};

}  // namespace fichero_printer
}  // namespace esphome

#endif  // USE_ESP32

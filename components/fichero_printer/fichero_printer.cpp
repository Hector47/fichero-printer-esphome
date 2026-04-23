/*
 * Fichero D11s thermal label printer – ESPHome external component
 *
 * Protocol:  AiYin D11s (LuckPrinter SDK), reverse-engineered from the
 *            Fichero APK (com.lj.fichero v1.1.5).
 *
 * Transport: BLE GATT – service 0x18F0, write 0x2AF1, notify 0x2AF0
 *
 * Print sequence (AiYin-specific):
 *   1. 10 FF 10 00 nn   Set density       (wait 100 ms)
 *   2. 10 FF 84 nn      Set paper type    (wait 50 ms)
 *   3. 00 * 12          Wake up           (wait 50 ms)
 *   4. 10 FF FE 01      Enable (AiYin)    (wait 50 ms)
 *   5. GS v 0 header + raster data        (wait 500 ms settle)
 *   6. 1D 0C            Form feed         (wait 300 ms)
 *   7. 10 FF FE 45      Stop (AiYin)  →  wait 0xAA or "OK" (60 s timeout)
 */

#include "fichero_printer.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace fichero_printer {

static const char *const TAG = "fichero_printer";

// ---------------------------------------------------------------------------
// BLEClientNode – GATT event handler
// ---------------------------------------------------------------------------

void FicheroPrinter::gattc_event_handler(esp_gattc_cb_event_t event,
                                          esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "BLE open failed (status=%d)", param->open.status);
        this->node_state_ = espbt::ClientState::IDLE;
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGI(TAG, "Printer disconnected");
      this->node_state_ = espbt::ClientState::IDLE;
      this->write_handle_ = 0;
      this->notify_handle_ = 0;
      this->reset_print_();
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (param->search_cmpl.conn_id != this->parent()->get_conn_id())
        break;
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Service discovery failed (status=%d)",
                 param->search_cmpl.status);
        break;
      }
      this->find_characteristics_();
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Failed to register for notify (status=%d)",
                 param->reg_for_notify.status);
        break;
      }
      ESP_LOGI(TAG, "Fichero printer ready");
      this->node_state_ = espbt::ClientState::ESTABLISHED;
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.conn_id != this->parent()->get_conn_id())
        break;
      if (param->notify.handle == this->notify_handle_ &&
          param->notify.value_len > 0) {
        this->response_received_ = true;
        this->response_byte_ = param->notify.value[0];
        ESP_LOGD(TAG, "Notification received: 0x%02X", this->response_byte_);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// GATT characteristic discovery
// ---------------------------------------------------------------------------

void FicheroPrinter::find_characteristics_() {
  esp_gatt_if_t gattc_if = this->parent()->get_gattc_if();
  uint16_t conn_id = this->parent()->get_conn_id();

  // Locate the 0x18F0 service to bound the handle search
  esp_bt_uuid_t svc_uuid;
  svc_uuid.len = ESP_UUID_LEN_16;
  svc_uuid.uuid.uuid16 = FICHERO_SERVICE_UUID;

  uint16_t count = 1;
  esp_gattc_service_elem_t svc_elem;
  esp_gatt_status_t status =
      esp_ble_gattc_get_service(gattc_if, conn_id, &svc_uuid, &svc_elem, &count, 0);

  uint16_t start_handle, end_handle;
  if (status == ESP_GATT_OK && count > 0) {
    start_handle = svc_elem.start_handle;
    end_handle = svc_elem.end_handle;
    ESP_LOGD(TAG, "Service 0x18F0 found: handles 0x%04X-0x%04X",
             start_handle, end_handle);
  } else {
    ESP_LOGW(TAG, "Service 0x18F0 not found, scanning full handle range");
    start_handle = 0x0001;
    end_handle = 0xFFFF;
  }

  esp_bt_uuid_t char_uuid;
  char_uuid.len = ESP_UUID_LEN_16;

  // Write characteristic (0x2AF1)
  char_uuid.uuid.uuid16 = WRITE_CHAR_UUID;
  uint16_t char_count = 1;
  esp_gattc_char_elem_t char_elem;
  status = esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id, start_handle,
                                           end_handle, char_uuid, &char_elem,
                                           &char_count);
  if (status == ESP_GATT_OK && char_count > 0) {
    this->write_handle_ = char_elem.char_handle;
    ESP_LOGD(TAG, "Write char handle: 0x%04X", this->write_handle_);
  } else {
    ESP_LOGW(TAG, "Write characteristic (0x2AF1) not found");
  }

  // Notify characteristic (0x2AF0)
  char_uuid.uuid.uuid16 = NOTIFY_CHAR_UUID;
  char_count = 1;
  status = esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id, start_handle,
                                           end_handle, char_uuid, &char_elem,
                                           &char_count);
  if (status == ESP_GATT_OK && char_count > 0) {
    this->notify_handle_ = char_elem.char_handle;
    ESP_LOGD(TAG, "Notify char handle: 0x%04X", this->notify_handle_);
    esp_ble_gattc_register_for_notify(
        gattc_if, this->parent()->get_remote_bda(), this->notify_handle_);
  } else {
    ESP_LOGW(TAG, "Notify characteristic (0x2AF0) not found");
  }
}

// ---------------------------------------------------------------------------
// Low-level BLE write (write-without-response)
// ---------------------------------------------------------------------------

void FicheroPrinter::write_bytes_(const uint8_t *data, size_t len) {
  if (this->write_handle_ == 0) {
    ESP_LOGW(TAG, "Write handle not available");
    return;
  }
  esp_err_t err = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
      this->write_handle_, len, const_cast<uint8_t *>(data),
      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "BLE write error: %d", err);
  }
}

// ---------------------------------------------------------------------------
// Public print API
// ---------------------------------------------------------------------------

void FicheroPrinter::print_raw(const std::vector<uint8_t> &data) {
  if (this->state_ != PrintState::IDLE) {
    ESP_LOGW(TAG, "Print already in progress, ignoring request");
    return;
  }
  if (this->node_state_ != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "Printer not connected");
    return;
  }
  if (data.empty() || (data.size() % BYTES_PER_ROW) != 0) {
    ESP_LOGW(TAG, "Invalid data size %d (must be a non-zero multiple of %d)",
             (int) data.size(), BYTES_PER_ROW);
    return;
  }

  this->raster_data_ = data;
  this->raster_rows_ = static_cast<uint16_t>(data.size() / BYTES_PER_ROW);
  this->raster_offset_ = 0;
  this->response_received_ = false;
  this->state_ = PrintState::SEND_DENSITY;
  this->state_deadline_ = 0;

  ESP_LOGI(TAG, "Starting print: %u rows, %u bytes", this->raster_rows_,
           (unsigned) data.size());
}

// ---------------------------------------------------------------------------
// Component loop – drives the state machine
// ---------------------------------------------------------------------------

void FicheroPrinter::loop() {
  if (this->state_ == PrintState::IDLE)
    return;

  if (this->node_state_ != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "Printer disconnected, aborting print");
    this->reset_print_();
    return;
  }

  // WAIT_STOP: poll for ACK notification or hard timeout
  if (this->state_ == PrintState::WAIT_STOP) {
    if (this->response_received_) {
      // The printer replies with either 0xAA (binary ACK) or the ASCII
      // string "OK".  Only the first notification byte is checked:
      //   0xAA  -> binary acknowledge
      //   'O' (0x4F) -> first byte of the "OK" ASCII response
      if (this->response_byte_ == 0xAA || this->response_byte_ == 'O') {
        ESP_LOGI(TAG, "Print complete");
      } else {
        ESP_LOGW(TAG, "Unexpected stop response: 0x%02X", this->response_byte_);
      }
      this->reset_print_();
    } else if (millis() >= this->state_deadline_) {
      ESP_LOGW(TAG, "Timeout waiting for print completion (60 s)");
      this->reset_print_();
    }
    return;
  }

  // All other states: wait for the inter-command delay
  if (this->state_deadline_ > 0 && millis() < this->state_deadline_)
    return;
  this->state_deadline_ = 0;

  this->advance_state_();
}

// ---------------------------------------------------------------------------
// State machine – advance one step
// ---------------------------------------------------------------------------

void FicheroPrinter::advance_state_() {
  switch (this->state_) {
    // 1. Set density
    case PrintState::SEND_DENSITY: {
      ESP_LOGD(TAG, "Setting density: %u", this->density_);
      uint8_t cmd[] = {0x10, 0xFF, 0x10, 0x00, this->density_};
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::SEND_PAPER;
      this->state_deadline_ = millis() + DELAY_AFTER_DENSITY_MS;
      break;
    }

    // 2. Set paper type
    case PrintState::SEND_PAPER: {
      ESP_LOGD(TAG, "Setting paper type: %u", this->paper_type_);
      uint8_t cmd[] = {0x10, 0xFF, 0x84, this->paper_type_};
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::SEND_WAKEUP;
      this->state_deadline_ = millis() + DELAY_COMMAND_GAP_MS;
      break;
    }

    // 3. Wake up (12 null bytes)
    case PrintState::SEND_WAKEUP: {
      ESP_LOGD(TAG, "Sending wakeup");
      uint8_t cmd[12] = {};
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::SEND_ENABLE;
      this->state_deadline_ = millis() + DELAY_COMMAND_GAP_MS;
      break;
    }

    // 4. Enable (AiYin-specific)
    case PrintState::SEND_ENABLE: {
      ESP_LOGD(TAG, "Enabling printer (AiYin)");
      uint8_t cmd[] = {0x10, 0xFF, 0xFE, 0x01};
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::SEND_RASTER;
      this->state_deadline_ = millis() + DELAY_COMMAND_GAP_MS;
      this->raster_offset_ = 0;
      break;
    }

    // 5. Send raster data in chunks
    case PrintState::SEND_RASTER: {
      if (this->raster_offset_ == 0) {
        // First write: GS v 0 header (8 bytes) + leading data
        uint8_t yl = this->raster_rows_ & 0xFF;
        uint8_t yh = (this->raster_rows_ >> 8) & 0xFF;

        // ESC/POS "GS v 0" raster command:
        //   1D 76 30 mm xL xH yL yH
        // 0x30 is the ASCII digit '0', which is the required mode byte for
        // "normal" print mode in the GS v 0 ESC/POS spec (not a typo).
        std::vector<uint8_t> pkt = {
            0x1D, 0x76, 0x30, 0x00,  // GS v 0, mode=normal
            BYTES_PER_ROW, 0x00,      // xL xH = 12, 0 (96 px / 8)
            yl, yh                    // yL yH = row count, little-endian
        };

        size_t data_in_first =
            std::min<size_t>(BLE_CHUNK_SIZE - 8, this->raster_data_.size());
        pkt.insert(pkt.end(), this->raster_data_.begin(),
                   this->raster_data_.begin() + data_in_first);

        this->write_bytes_(pkt.data(), pkt.size());
        this->raster_offset_ = data_in_first;

        ESP_LOGD(TAG, "Raster header+first chunk: %u bytes (offset %u/%u)",
                 (unsigned) pkt.size(), (unsigned) this->raster_offset_,
                 (unsigned) this->raster_data_.size());
      } else {
        // Subsequent chunks
        size_t remaining = this->raster_data_.size() - this->raster_offset_;
        size_t chunk = std::min<size_t>(BLE_CHUNK_SIZE, remaining);
        this->write_bytes_(this->raster_data_.data() + this->raster_offset_,
                           chunk);
        this->raster_offset_ += chunk;

        ESP_LOGD(TAG, "Raster chunk: offset %u/%u",
                 (unsigned) this->raster_offset_,
                 (unsigned) this->raster_data_.size());
      }

      if (this->raster_offset_ >= this->raster_data_.size()) {
        // All raster data sent; wait for printhead to settle
        ESP_LOGD(TAG, "Raster complete, settling...");
        this->state_ = PrintState::SEND_FORM_FEED;
        this->state_deadline_ = millis() + DELAY_RASTER_SETTLE_MS;
      } else {
        // More chunks pending; small inter-chunk gap
        this->state_deadline_ = millis() + DELAY_CHUNK_GAP_MS;
      }
      break;
    }

    // 6. Form feed
    case PrintState::SEND_FORM_FEED: {
      ESP_LOGD(TAG, "Form feed");
      uint8_t cmd[] = {0x1D, 0x0C};
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::SEND_STOP;
      this->state_deadline_ = millis() + DELAY_AFTER_FEED_MS;
      break;
    }

    // 7. Stop print (AiYin-specific) – now wait for ACK in loop()
    case PrintState::SEND_STOP: {
      ESP_LOGD(TAG, "Sending stop command, awaiting ACK...");
      uint8_t cmd[] = {0x10, 0xFF, 0xFE, 0x45};
      this->response_received_ = false;
      this->write_bytes_(cmd, sizeof(cmd));
      this->state_ = PrintState::WAIT_STOP;
      this->state_deadline_ = millis() + PRINT_TIMEOUT_MS;
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void FicheroPrinter::reset_print_() {
  this->state_ = PrintState::IDLE;
  this->state_deadline_ = 0;
  this->raster_data_.clear();
  this->raster_offset_ = 0;
  this->response_received_ = false;
}

}  // namespace fichero_printer
}  // namespace esphome

#endif  // USE_ESP32

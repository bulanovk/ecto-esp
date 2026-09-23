#include "baud_router.h"

#include "esphome/core/log.h"

namespace esphome::baud_router {

static const char *const TAG = "baud_router";

void BaudRouter::setup() {
  if (this->real_ == nullptr) {
    ESP_LOGE(TAG, "No real UART bound (uart_id: missing)");
    this->mark_failed();
    return;
  }

  // Mirror the real port's RX thresholds. The hub reads these from US (we are its `parent_`), not from
  // the port they actually configure: `Modbus::setup()` derives `long_rx_buffer_delay_us_` from
  // `get_rx_full_threshold()` and `rx_detect_latency_us_` from `get_rx_timeout()`. Left at the base
  // defaults the first falls back to 50 ms (instead of ~8 ms at 9600) and the second to 0, so the
  // parser waits far too long for a frame to look complete. Our own `setup()` runs at BUS - 0.2 and
  // the hub's at BUS - 1.0, so copying here lands before the hub reads them.
  this->rx_full_threshold_ = this->real_->get_rx_full_threshold();
  this->rx_timeout_ = this->real_->get_rx_timeout();

  // Rule 1, enforced in code: the hub derives frame_delay_us_ (3.5 characters) ONCE, in Modbus::setup(),
  // from the getters of whatever UARTComponent it was handed - i.e. from our baud_rate_. That delay must
  // cover the SLOWEST device on the wire, so our rate must be <= every routed rate. A faster value here
  // shortens the inter-frame gap below 3.5 characters for the slow devices and splices their frames.
  uint32_t slowest = this->default_baud_;
  for (const auto &route : this->routes_) {
    if (route.second < slowest)
      slowest = route.second;
  }
  if (slowest != 0 && this->baud_rate_ > slowest) {
    ESP_LOGW(TAG, "baud_rate %" PRIu32 " is faster than the slowest device (%" PRIu32
                  "); frame delay will be too short and frames may splice. Set baud_rate to the "
                  "fleet minimum.",
             this->baud_rate_, slowest);
  }
}

void BaudRouter::dump_config() {
  ESP_LOGCONFIG(TAG, "Baud Router:");
  ESP_LOGCONFIG(TAG, "  Frame delay rate: %" PRIu32 " (fleet minimum)", this->baud_rate_);
  ESP_LOGCONFIG(TAG, "  Default rate: %" PRIu32 " (addresses not in map)", this->default_baud_);
  for (const auto &route : this->routes_) {
    ESP_LOGCONFIG(TAG, "  0x%02X -> %" PRIu32, route.first, route.second);
  }
}

uint32_t BaudRouter::baud_for_address(uint8_t address) {
  for (const auto &route : this->routes_) {
    if (route.first == address)
      return route.second;
  }
  // Not routed: almost always a typo in a modbus_controller `address:`. Warn once, not per frame.
  if (!this->warned_unknown_) {
    ESP_LOGW(TAG, "Address 0x%02X is not in the map; using default %" PRIu32
                  ". Check the address for typos.",
             address, this->default_baud_);
    this->warned_unknown_ = true;
  }
  return this->default_baud_;
}

void BaudRouter::write_array(const uint8_t *data, size_t len) {
  if (len == 0)
    return;

  // The frame's first byte is the slave address; the speed is looked up from it, so the frame is never
  // parsed. This is the only point in the send path where the frame bytes are available - the hub's
  // own virtuals either run before the frame is chosen or receive no frame at all.
  const uint32_t target = this->baud_for_address(data[0]);

  // Reinstall the driver ONLY when the line settings actually differ from the live port. Two cases this
  // covers: consecutive frames for devices on the same speed (the common case - the hub's queue groups
  // them, so a whole polling sweep costs at most one switch per group boundary), and a return to a speed
  // after visiting another. Comparing against the live port rather than a saved "last speed" flag means
  // there is no state to resynchronise after boot, shutdown, or a foreign driver reinstall.
  const bool line_changed = this->real_->get_baud_rate() != target ||
                            this->real_->get_data_bits() != this->data_bits_ ||
                            this->real_->get_stop_bits() != this->stop_bits_ ||
                            this->real_->get_parity() != this->parity_;

  if (line_changed) {
    ESP_LOGV(TAG, "Switching line to %" PRIu32 " for address 0x%02X", target, data[0]);
    this->real_->set_baud_rate(target);
    this->real_->set_data_bits(this->data_bits_);
    this->real_->set_stop_bits(this->stop_bits_);
    this->real_->set_parity(this->parity_);
    // Reinstalls the driver: delete + install + param_config + set_pin + set_mode. Because
    // uart_driver_install resets the peripheral, uart_set_mode() is re-applied here too, so a
    // flow_control_pin configured on the real `uart:` keeps its RS485 half-duplex mode across
    // switches. A fresh RX ring also discards anything received at the old speed.
    this->real_->load_settings(false);
  }

  // TX ring size is 0, so this blocks until the frame is on the wire - no flush is needed, and the
  // frame cannot straddle a later speed change.
  this->real_->write_array(data, len);
}

}  // namespace esphome::baud_router

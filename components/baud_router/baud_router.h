#pragma once

#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

namespace esphome::baud_router {

/// A `UARTComponent` that fronts a real UART and switches its baud rate per frame.
///
/// The modbus hub only ever sees this class. `Modbus::setup()` derives `frame_delay_us_` and
/// `bits_per_char_` from the getters, so keeping the router's own `baud_rate_` at the **fleet
/// minimum** makes the inter-frame delay long enough for every device on the wire. The live port
/// speed is never read by the hub at runtime, so switching it per frame is invisible to it.
///
/// The speed is chosen from the frame's **first byte - the slave address** - so no frame parsing is
/// needed: `data[0]` is the address, `data[1]` the function code.
class BaudRouter : public uart::UARTComponent, public Component {
 public:
  /// The real UART this router owns. Set from Python (`uart_id:`).
  void set_real(uart::UARTComponent *real) { this->real_ = real; }

  /// Baud rate used for an address that is not in `map:`. Set from Python.
  void set_default_baud(uint32_t default_baud) { this->default_baud_ = default_baud; }

  /// Register one `address -> baud_rate` route. Called once per `map:` entry at codegen time.
  void add_route(uint8_t address, uint32_t baud_rate) {
    this->routes_.emplace_back(address, baud_rate);
  }

  void setup() override;
  void dump_config() override;

  // ── UARTComponent: the six pure virtuals ──────────────────────────────────────────────────────
  /// The one place where "moment of send" and "frame bytes" coincide, and the only virtual seam that
  /// has both. See the design note in `docs/baud-proxy-design.md` §10.4.
  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override { return this->real_->peek_byte(data); }
  bool read_array(uint8_t *data, size_t len) override { return this->real_->read_array(data, len); }
  size_t available() override { return this->real_->available(); }
  uart::UARTFlushResult flush() override { return this->real_->flush(); }

#if defined(USE_ESP8266) || defined(USE_ESP32)
  void load_settings(bool dump_config) override { this->real_->load_settings(dump_config); }
  using UARTComponent::load_settings;  // un-hide the no-arg overload
#endif

  /// Apply to the real port AND keep a local copy. The local copy is the one that matters: the hub
  /// reads these off us, not off the port they configure. (`uart:` writes the real port's copy
  /// directly, so `setup()` mirrors it here as well.)
  void set_rx_full_threshold(size_t rx_full_threshold) override {
    this->rx_full_threshold_ = rx_full_threshold;
    this->real_->set_rx_full_threshold(rx_full_threshold);
  }
  void set_rx_timeout(size_t rx_timeout) override {
    this->rx_timeout_ = rx_timeout;
    this->real_->set_rx_timeout(rx_timeout);
  }

  /// Baud rate for a slave address: `map:` first, then `default_baud_`.
  uint32_t baud_for_address(uint8_t address);

 protected:
  float get_setup_priority() const override {
    // After the real UART (BUS) so its driver exists, before the modbus hub (BUS - 1.0) so the
    // router's settings are in place by the time the hub derives its delays from them.
    return setup_priority::BUS - 0.2f;
  }

  /// The real UART owns the logger conflict check; the router has no pins of its own.
  void check_logger_conflict() override {}

  uart::UARTComponent *real_{nullptr};
  std::vector<std::pair<uint8_t, uint32_t>> routes_{};
  uint32_t default_baud_{0};
  bool warned_unknown_{false};  ///< so an address missing from `map:` logs once, not per frame
};

}  // namespace esphome::baud_router

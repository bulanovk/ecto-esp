#pragma once

#include <map>
#include <span>

#include "esphome/components/modbus_controller/modbus_controller.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::bit_modbus_switch {

/// Per-address state shared by every bit-switch on one holding register: the write semaphore
/// (mirrors the HA integration's per-register RMW lock) and the latest register value. All
/// switches on one address parse the SAME poll response (modbus_controller forces same-address
/// items to share one polling range, modbus_controller.cpp try_share()), so last_value is one
/// coherent value, not ten drifting copies.
struct RegisterState {
  esphome::Mutex mutex;
  uint16_t last_value{0};
  bool known{false};  ///< false until the first poll response.
};

/// Registry of per-address states. Static on purpose: switch instances are created per config
/// entry and cannot know about each other; the address is the natural key.
RegisterState *get_register_state(uint16_t address);

/// A `ModbusSwitch` copy (the stock class is final) with the write path turned into a
/// mutex-guarded read-modify-write:
///
///   lock → merged = last_value | / & ~bitmask → write_single_register(merged) → last_value = merged → unlock
///
/// Everything else — PDU construction, hub queueing (FC 0x06), parse_and_publish — is the stock
/// switch's. The registry is touched in exactly three places:
///   * parse_and_publish: poll response feeds last_value (+known),
///   * write_state: reads last_value for the merge,
///   * write_state: writes last_value = merged, still under the lock and before unlock.
/// A poll every update_interval re-syncs the registry with the device (failed writes, panel
/// buttons, relay timers).
class BitModbusSwitch final : public Component, public switch_::Switch, public modbus_controller::SensorItem,
                             public modbus_controller::WriterEntity {
 public:
  BitModbusSwitch(modbus::EntityType register_type, uint16_t start_address, uint8_t offset, uint32_t bitmask,
                  modbus_controller::RangeReuse reuse_previous_range) {
    this->register_type = register_type;
    this->set_address(start_address);
    this->set_offset_from_start_address(offset);
    this->bitmask = bitmask;
    this->sensor_value_type = modbus_controller::SensorValueType::BIT;
    // Same fold as the stock switch: a holding byte offset becomes whole registers in the address.
    if (register_type == modbus::EntityType::HOLDING) {
      this->set_address(start_address + offset / 2);
      this->set_offset_from_start_address(0);
    }
    this->reuse_previous_range = reuse_previous_range;
  }

  void set_parent(modbus_controller::ModbusController *parent) { this->set_controller_(parent); }
  void set_use_write_multiple(bool use_write_multiple) { this->use_write_multiple_ = use_write_multiple; }

  void setup() override;
  void write_state(bool state) override;
  void dump_config() override;
  void parse_and_publish(std::span<const uint8_t> data) override;

 protected:
  bool assumed_state() override { return false; }

  RegisterState *reg_{nullptr};
  /// false -> FC 0x06 (Write Single Register), true -> FC 0x10 (Write Multiple Registers, one register).
  /// The ectoControl relay blocks answer only FC 0x10 (see docs/baud-proxy-design.md §6), so the relay
  /// switches set this true; the stock switch's default is false and is kept here for parity.
  bool use_write_multiple_{false};
};

}  // namespace esphome::bit_modbus_switch

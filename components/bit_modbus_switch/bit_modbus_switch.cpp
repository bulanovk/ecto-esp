#include "bit_modbus_switch.h"

#include "esphome/core/log.h"

#include <array>

namespace esphome::bit_modbus_switch {

static const char *const TAG = "bit_modbus_switch";

RegisterState *get_register_state(uint16_t address) {
  static std::map<uint16_t, RegisterState> registry;
  return &registry[address];
}

void BitModbusSwitch::setup() {
  this->reg_ = get_register_state(this->write_address());
}

void BitModbusSwitch::write_state(bool state) {
  if (this->reg_ == nullptr)
    this->reg_ = get_register_state(this->write_address());
  LockGuard guard(this->reg_->mutex);
  if (!this->reg_->known) {
    // No poll response yet: writing a guessed value could clobber the other 9 channels.
    ESP_LOGW(TAG, "'%s': register 0x%04X value unknown yet, refusing write", this->get_name().c_str(),
             this->write_address());
    return;
  }
  const uint16_t current = this->reg_->last_value;
  const uint16_t merged = state ? (current | this->bitmask) : (current & ~this->bitmask);
  if (merged == current) {
    ESP_LOGD(TAG, "'%s': no change, skipping write", this->get_name().c_str());
    this->publish_state(state);
    return;
  }
  ESP_LOGD(TAG, "'%s': register 0x%04X: 0x%04X -> 0x%04X (bit 0x%04X %s)", this->get_name().c_str(),
           this->write_address(), current, merged, (unsigned) this->bitmask, ONOFF(state));
  // FC 0x10 carries the same register value in a different frame shape; the relay blocks answer only
  // that one, so the merge result is identical either way and only the function code differs.
  bool queued;
  if (this->use_write_multiple_) {
    const std::array<uint16_t, 1> values{merged};
    queued = this->write_multiple_registers(this->write_address(), values);
  } else {
    queued = this->write_single_register(this->write_address(), merged);
  }
  if (!queued) {
    ESP_LOGW(TAG, "'%s': write refused by hub, state not published", this->get_name().c_str());
    return;
  }
  // Optimistic registry update, still under the lock: a second switch toggled in this same loop
  // cycle must read the merged value, not the pre-write snapshot.
  this->reg_->last_value = merged;
  this->publish_state(state);
}

void BitModbusSwitch::parse_and_publish(std::span<const uint8_t> data) {
  if (this->reg_ == nullptr)
    this->reg_ = get_register_state(this->write_address());
  LockGuard guard(this->reg_->mutex);
  const uint16_t value = modbus::helpers::get_data<uint16_t>(data.data(), this->offset);
  this->reg_->last_value = value;
  this->reg_->known = true;
  this->publish_state(value & this->bitmask);
}

void BitModbusSwitch::dump_config() {
  LOG_SWITCH(TAG, "Bit Modbus Switch", this);
  ESP_LOGCONFIG(TAG, "  Address: 0x%04X, bitmask 0x%04X", this->write_address(), (unsigned) this->bitmask);
}

}  // namespace esphome::bit_modbus_switch

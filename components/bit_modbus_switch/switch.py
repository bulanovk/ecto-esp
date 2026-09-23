"""Schema/codegen for the bit_modbus_switch switch platform.

A copy of modbus_controller/switch/__init__.py differing only in the C++ class
(bit_modbus_switch::BitModbusSwitch) and the required holding-only register_type
with an explicit bitmask.
"""

import esphome.codegen as cg
from esphome.components import modbus, modbus_controller, switch
from esphome.components.modbus.helpers import MODBUS_REGISTER_TYPE
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID
from esphome.types import ConfigType

from esphome.components.modbus_controller import (
    RANGE_REUSE,
    ModbusItemBaseSchema,
    add_modbus_base_properties,
    modbus_calc_properties,
    reject_odd_holding_write_offset,
    validate_modbus_register,
)
from esphome.components.modbus_controller.const import (
    CONF_BITMASK,
    CONF_MODBUS_CONTROLLER_ID,
    CONF_REGISTER_TYPE,
    CONF_REUSE_PREVIOUS_RANGE,
    CONF_USE_WRITE_MULTIPLE,
)

DEPENDENCIES = ["modbus_controller"]

bit_modbus_switch_ns = cg.esphome_ns.namespace("bit_modbus_switch")
BitModbusSwitch = bit_modbus_switch_ns.class_(
    "BitModbusSwitch", cg.Component, switch.Switch, modbus_controller.SensorItem
)


def _validate_holding_offset(config: ConfigType) -> ConfigType:
    # A 16-bit register write cannot target half a register.
    if config.get(CONF_REGISTER_TYPE) == "holding":
        reject_odd_holding_write_offset(config)
    return config


CONFIG_SCHEMA = cv.All(
    switch.switch_schema(BitModbusSwitch, default_restore_mode="DISABLED")
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ModbusItemBaseSchema)
    .extend(
        {
            cv.Required(CONF_REGISTER_TYPE): cv.enum(MODBUS_REGISTER_TYPE),
            cv.Required(CONF_BITMASK): cv.hex_uint16_t,
            # false -> FC 0x06, true -> FC 0x10 (one register). The ectoControl relay blocks answer
            # only FC 0x10, so a relay switch must set this true or the write times out.
            cv.Optional(CONF_USE_WRITE_MULTIPLE, default=False): cv.boolean,
        }
    ),
    validate_modbus_register,
    _validate_holding_offset,
)


async def to_code(config: ConfigType) -> None:
    byte_offset = modbus_calc_properties(config)
    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_REGISTER_TYPE],
        config[CONF_ADDRESS],
        byte_offset,
        config[CONF_BITMASK],
        RANGE_REUSE[config[CONF_REUSE_PREVIOUS_RANGE]],
    )
    await cg.register_component(var, config)
    await switch.register_switch(var, config)

    paren = await cg.get_variable(config[CONF_MODBUS_CONTROLLER_ID])
    cg.add(var.set_parent(paren))
    cg.add(var.set_use_write_multiple(config[CONF_USE_WRITE_MULTIPLE]))
    cg.add(paren.add_sensor_item(var))
    await add_modbus_base_properties(var, config, BitModbusSwitch, cg.bool_, bool)

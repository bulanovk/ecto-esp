import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import uart
from esphome.components.const import CONF_DATA_BITS, CONF_PARITY, CONF_STOP_BITS
from esphome.const import CONF_BAUD_RATE, CONF_ID, CONF_TX_PIN, CONF_RX_PIN
from esphome.types import ConfigType

CODEOWNERS = ["@local"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["uart"]
MULTI_CONF = True

CONF_MAP = "map"
CONF_DEFAULT_BAUD = "default_baud"

baud_router_ns = cg.esphome_ns.namespace("baud_router")
BaudRouter = baud_router_ns.class_("BaudRouter", uart.UARTComponent, cg.Component)

# One `address -> baud_rate` entry. A list rather than a dict because ESPHome schemas are keyed by
# string, so an `address:` field carries the int and the map stays readable in YAML.
ROUTE_SCHEMA = cv.Schema(
    {
        cv.Required("address"): cv.int_range(min=1, max=247),
        cv.Required(CONF_BAUD_RATE): cv.int_range(min=1),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BaudRouter),
            # The REAL uart. This component is a UARTComponent itself, so it must never be pointed at
            # another router - `uart_id:` resolving to one is caught in final_validate below.
            cv.Required(uart.CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Required(CONF_BAUD_RATE): cv.int_range(min=1),
            cv.Optional(CONF_DEFAULT_BAUD): cv.int_range(min=1),
            cv.Required(CONF_MAP): cv.ensure_list(ROUTE_SCHEMA),
            cv.Optional(CONF_DATA_BITS, default=8): cv.one_of(8, int=True),
            cv.Optional(CONF_STOP_BITS, default=1): cv.one_of(1, 2, int=True),
            cv.Optional(CONF_PARITY, default="NONE"): cv.enum(uart.UART_PARITY_OPTIONS, upper=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,  # load_settings/rx_full_threshold live in IDFUARTComponent
)


def _validate_routes(config: ConfigType) -> ConfigType:
    """Catch an address routed twice, and the two ways the fleet-minimum rule can be violated."""
    seen: set[int] = set()
    slowest = config.get(CONF_DEFAULT_BAUD, config[CONF_BAUD_RATE])
    for route in config[CONF_MAP]:
        address = route["address"]
        if address in seen:
            raise cv.Invalid(f"Address 0x{address:02X} is routed more than once")
        seen.add(address)
        slowest = min(slowest, route[CONF_BAUD_RATE])

    # Rule 1: the hub computes frame_delay_us_ (3.5 chars) once, from THIS component's baud_rate.
    # It must therefore be the fleet minimum, or the inter-frame gap is too short for the slow
    # devices and their frames splice into CRC errors.
    if config[CONF_BAUD_RATE] > slowest:
        raise cv.Invalid(
            f"baud_rate {config[CONF_BAUD_RATE]} is faster than the slowest routed device "
            f"({slowest}). frame_delay_us_ is derived once from baud_rate, so it must be the fleet "
            f"minimum - otherwise frames on the slower devices splice."
        )
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_routes)


def _validate_real_uart(hub_config: ConfigType) -> ConfigType:
    """`uart_id:` must resolve to a real `uart:`, never to another router.

    A router is itself a UARTComponent, so nothing stops a second router from being chained onto it -
    and the chain would silently double the address lookup. `id_declaration_match_schema` hands us the
    referenced ID's *declaration*, which is where the real pin keys live.
    """
    if CONF_TX_PIN not in hub_config and CONF_RX_PIN not in hub_config:
        raise cv.Invalid(
            "uart_id: must point at a real uart: (with tx_pin/rx_pin), not at another baud_router"
        )
    return hub_config


# This is a filter over the already-validated config, NOT the component's schema - so it must allow
# every other key through. Without ALLOW_EXTRA it rejects baud_rate/map/... as unknown options, and
# the error is reported against `baud_router:` as a whole, which reads like a CONFIG_SCHEMA problem.
FINAL_VALIDATE_SCHEMA = cv.Schema(
    {cv.Required(uart.CONF_UART_ID): fv.id_declaration_match_schema(_validate_real_uart)},
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    real = await cg.get_variable(config[uart.CONF_UART_ID])
    cg.add(var.set_real(real))

    cg.add(var.set_baud_rate(config[CONF_BAUD_RATE]))
    cg.add(var.set_data_bits(config[CONF_DATA_BITS]))
    cg.add(var.set_stop_bits(config[CONF_STOP_BITS]))
    cg.add(var.set_parity(config[CONF_PARITY]))

    default_baud = config.get(CONF_DEFAULT_BAUD, config[CONF_BAUD_RATE])
    cg.add(var.set_default_baud(default_baud))
    for route in config[CONF_MAP]:
        cg.add(var.add_route(route["address"], route[CONF_BAUD_RATE]))

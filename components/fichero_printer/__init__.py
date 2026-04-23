"""ESPHome external component for Fichero D11s thermal label printer.

Protocol reverse-engineered from the Fichero APK (AiYin D11s device class).
96-pixel-wide printhead, 203 DPI, 1-bit raster images over BLE GATT.
See https://github.com/Hector47/fichero-printer for the Python reference impl.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import ble_client
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client"]
CODEOWNERS = ["@Hector47"]

fichero_printer_ns = cg.esphome_ns.namespace("fichero_printer")

FicheroPrinter = fichero_printer_ns.class_(
    "FicheroPrinter", cg.Component, ble_client.BLEClientNode
)

PrintAction = fichero_printer_ns.class_("PrintAction", automation.Action)

CONF_DENSITY = "density"
CONF_PAPER_TYPE = "paper_type"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FicheroPrinter),
            cv.Optional(CONF_DENSITY, default=1): cv.int_range(min=0, max=2),
            cv.Optional(CONF_PAPER_TYPE, default=0): cv.int_range(min=0, max=2),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_density(config[CONF_DENSITY]))
    cg.add(var.set_paper_type(config[CONF_PAPER_TYPE]))


# ---------------------------------------------------------------------------
# fichero_printer.print action
# ---------------------------------------------------------------------------

PRINT_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ID): cv.use_id(FicheroPrinter),
        cv.Required("data"): cv.templatable(
            cv.ensure_list(cv.All(cv.int_, cv.int_range(min=0, max=255)))
        ),
    }
)


@automation.register_action(
    "fichero_printer.print",
    PrintAction,
    PRINT_ACTION_SCHEMA,
)
async def fichero_print_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])

    data = await cg.templatable(
        config["data"], args, cg.std_vector.template(cg.uint8)
    )
    cg.add(var.set_data(data))
    return var

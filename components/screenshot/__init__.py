import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_WIDTH, CONF_HEIGHT
from esphome.components import web_server_base

CODEOWNERS = ["@NickLD"]
DEPENDENCIES = ["web_server_base", "lvgl"]
AUTO_LOAD = []

screenshot_ns = cg.esphome_ns.namespace("screenshot")
ScreenshotComponent = screenshot_ns.class_("ScreenshotComponent", cg.Component)

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"


def _width_multiple_of_4(value):
    value = cv.positive_int(value)
    if value % 4 != 0:
        raise cv.Invalid("width must be a multiple of 4 (BMP rows are 4-byte aligned; this encoder emits no row padding)")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ScreenshotComponent),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_WIDTH): _width_multiple_of_4,
        cv.Required(CONF_HEIGHT): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(base))
    cg.add_define("SCREENSHOT_SCREEN_W", config[CONF_WIDTH])
    cg.add_define("SCREENSHOT_SCREEN_H", config[CONF_HEIGHT])

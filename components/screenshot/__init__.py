import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import web_server_base

CODEOWNERS = ["@NickLD"]
DEPENDENCIES = ["web_server_base", "lvgl"]
AUTO_LOAD = []

screenshot_ns = cg.esphome_ns.namespace("screenshot")
ScreenshotComponent = screenshot_ns.class_("ScreenshotComponent", cg.Component)

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ScreenshotComponent),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
        cv.Required(CONF_WIDTH): cv.positive_int,
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

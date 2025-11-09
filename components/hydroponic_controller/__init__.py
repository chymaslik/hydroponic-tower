import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server, fan, light, time
from esphome.const import CONF_ID

AUTO_LOAD = ["web_server", "fan", "light", "time"]
CODEOWNERS = ["@hydroponic"]

CONF_PUMP_ID = "pump_id"
CONF_LIGHT_ID = "light_id"
CONF_TIME_ID = "time_id"
CONF_WEB_SERVER_ID = "web_server_id"
CONF_ON_MINUTES = "on_minutes"
CONF_OFF_MINUTES = "off_minutes"
CONF_ENABLED = "enabled"

hydroponic_controller_ns = cg.esphome_ns.namespace("hydroponic_controller")
HydroponicController = hydroponic_controller_ns.class_("HydroponicController", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(HydroponicController),
    cv.Required(CONF_PUMP_ID): cv.use_id(fan.Fan),
    cv.Required(CONF_LIGHT_ID): cv.use_id(light.LightState),
    cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Required(CONF_WEB_SERVER_ID): cv.use_id(web_server.WebServer),
    cv.Optional(CONF_ON_MINUTES, default=5): cv.int_range(min=1, max=120),
    cv.Optional(CONF_OFF_MINUTES, default=15): cv.int_range(min=1, max=120),
    cv.Optional(CONF_ENABLED, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pump = await cg.get_variable(config[CONF_PUMP_ID])
    light_ = await cg.get_variable(config[CONF_LIGHT_ID])
    time_ = await cg.get_variable(config[CONF_TIME_ID])
    server = await cg.get_variable(config[CONF_WEB_SERVER_ID])

    cg.add(var.set_pump(pump))
    cg.add(var.set_light(light_))
    cg.add(var.set_clock(time_))
    cg.add(var.set_server(server))
    cg.add(var.set_durations(config[CONF_ON_MINUTES], config[CONF_OFF_MINUTES]))
    cg.add(var.set_enabled(config[CONF_ENABLED]))

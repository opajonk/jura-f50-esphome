import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_UPDATE_INTERVAL

from . import JuraF50

DEPENDENCIES = ["jura_f50"]

CONF_JURA_F50_ID = "jura_f50_id"
CONF_CLOCK = "clock"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_JURA_F50_ID): cv.use_id(JuraF50),
        cv.Required(CONF_CLOCK): text_sensor.text_sensor_schema(icon="mdi:clock-outline"),
        cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_JURA_F50_ID])
    entity = await text_sensor.new_text_sensor(config[CONF_CLOCK])
    cg.add(hub.set_clock_text_sensor(entity))
    cg.add(hub.set_clock_interval_ms(config[CONF_UPDATE_INTERVAL]))

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_UPDATE_INTERVAL, DEVICE_CLASS_POWER

from . import JuraF50

DEPENDENCIES = ["jura_f50"]

CONF_JURA_F50_ID = "jura_f50_id"
CONF_POWER = "power"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_JURA_F50_ID): cv.use_id(JuraF50),
        cv.Required(CONF_POWER): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_POWER, icon="mdi:coffee-maker"
        ),
        cv.Optional(CONF_UPDATE_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_JURA_F50_ID])
    sensor = await binary_sensor.new_binary_sensor(config[CONF_POWER])
    cg.add(hub.set_power_binary_sensor(sensor))
    cg.add(hub.set_status_interval_ms(config[CONF_UPDATE_INTERVAL]))

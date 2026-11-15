import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_UPDATE_INTERVAL, STATE_CLASS_TOTAL_INCREASING

from . import JuraF50

DEPENDENCIES = ["jura_f50"]

CONF_JURA_F50_ID = "jura_f50_id"
CONF_SINGLE_COFFEE = "single_coffee"
CONF_TOTAL_COFFEE = "total_coffee"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_JURA_F50_ID): cv.use_id(JuraF50),
            cv.Optional(CONF_SINGLE_COFFEE): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:coffee",
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional(CONF_TOTAL_COFFEE): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:coffee",
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional(CONF_UPDATE_INTERVAL, default="120s"): cv.positive_time_period_milliseconds,
        }
    ),
    cv.has_at_least_one_key(CONF_SINGLE_COFFEE, CONF_TOTAL_COFFEE),
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_JURA_F50_ID])
    if CONF_SINGLE_COFFEE in config:
        entity = await sensor.new_sensor(config[CONF_SINGLE_COFFEE])
        cg.add(hub.set_single_coffee_sensor(entity))
    if CONF_TOTAL_COFFEE in config:
        entity = await sensor.new_sensor(config[CONF_TOTAL_COFFEE])
        cg.add(hub.set_total_coffee_sensor(entity))
    cg.add(hub.set_counter_interval_ms(config[CONF_UPDATE_INTERVAL]))

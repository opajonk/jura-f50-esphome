import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["uart"]
CODEOWNERS = []

CONF_ON_LINE = "on_line"
CONF_ON_RESPONSE = "on_response"
CONF_INTRA_BYTE_DELAY = "intra_byte_delay"
CONF_INTER_FRAME_DELAY = "inter_frame_delay"
CONF_RX_TIMEOUT = "rx_timeout"

jura_f50_ns = cg.esphome_ns.namespace("jura_f50")
JuraF50 = jura_f50_ns.class_("JuraF50", cg.Component, uart.UARTDevice)
LineTrigger = jura_f50_ns.class_("LineTrigger", automation.Trigger.template(cg.std_string))
ResponseTrigger = jura_f50_ns.class_(
    "ResponseTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(JuraF50),
            cv.Optional(CONF_INTRA_BYTE_DELAY, default="1ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_INTER_FRAME_DELAY, default="7ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_RX_TIMEOUT, default="20ms"): cv.positive_time_period_microseconds,
            cv.Optional(CONF_ON_LINE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LineTrigger)}
            ),
            cv.Optional(CONF_ON_RESPONSE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ResponseTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "jura_f50", baud_rate=9600, require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_intra_byte_delay_us(config[CONF_INTRA_BYTE_DELAY]))
    cg.add(var.set_inter_frame_delay_us(config[CONF_INTER_FRAME_DELAY]))
    cg.add(var.set_rx_timeout_us(config[CONF_RX_TIMEOUT]))
    for conf in config.get(CONF_ON_LINE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "line")], conf)
    for conf in config.get(CONF_ON_RESPONSE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "command"), (cg.std_string, "response")], conf
        )

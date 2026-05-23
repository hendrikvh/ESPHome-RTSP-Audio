import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_BYTES,
)

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent

DEPENDENCIES = ["rtsp_audio"]

CONF_BYTES_SENT = "bytes_sent"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RTSP_AUDIO_ID): cv.use_id(RtspAudioComponent),
        cv.Optional(CONF_BYTES_SENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_BYTES,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RTSP_AUDIO_ID])
    if conf := config.get(CONF_BYTES_SENT):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_bytes_sent_sensor(sens))

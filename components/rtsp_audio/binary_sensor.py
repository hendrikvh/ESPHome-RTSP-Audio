import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent

DEPENDENCIES = ["rtsp_audio"]

CONF_CLIENT_CONNECTED = "client_connected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RTSP_AUDIO_ID): cv.use_id(RtspAudioComponent),
        cv.Optional(CONF_CLIENT_CONNECTED): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RTSP_AUDIO_ID])
    if conf := config.get(CONF_CLIENT_CONNECTED):
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(parent.set_client_connected_binary_sensor(sens))

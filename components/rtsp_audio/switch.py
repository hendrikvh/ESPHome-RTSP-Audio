import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent, rtsp_audio_ns

DEPENDENCIES = ["rtsp_audio"]

CONF_SOFT_LIMITER_ENABLED = "soft_limiter_enabled"

RtspAudioSoftLimiterSwitch = rtsp_audio_ns.class_(
    "RtspAudioSoftLimiterSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RTSP_AUDIO_ID): cv.use_id(RtspAudioComponent),
        cv.Optional(CONF_SOFT_LIMITER_ENABLED): switch.switch_schema(
            RtspAudioSoftLimiterSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="RESTORE_DEFAULT_OFF",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RTSP_AUDIO_ID])
    if conf := config.get(CONF_SOFT_LIMITER_ENABLED):
        var = await switch.new_switch(conf)
        await cg.register_component(var, conf)
        cg.add(var.set_parent(parent))

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_INITIAL_VALUE,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_RESTORE_VALUE,
    CONF_STEP,
    ENTITY_CATEGORY_CONFIG,
    UNIT_HERTZ,
)

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent, rtsp_audio_ns

DEPENDENCIES = ["rtsp_audio"]

CONF_LOWCUT_FILTER_FREQUENCY = "lowcut_filter_frequency"

# Mirrors dc_blocker.h: DC_BLOCKER_DEFAULT_CUTOFF_HZ, MIN_CUTOFF_HZ,
# MAX_CUTOFF_HZ. The C++ side also clamps internally, but having the
# bounds here means HA only ever sees valid values on the slider.
DEFAULT_CUTOFF = 100.0
MIN_CUTOFF = 20.0
MAX_CUTOFF = 500.0
DEFAULT_STEP = 10.0

RtspAudioLowCutFilterNumber = rtsp_audio_ns.class_(
    "RtspAudioLowCutFilterNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RTSP_AUDIO_ID): cv.use_id(RtspAudioComponent),
        cv.Optional(CONF_LOWCUT_FILTER_FREQUENCY): number.number_schema(
            RtspAudioLowCutFilterNumber,
            unit_of_measurement=UNIT_HERTZ,
            entity_category=ENTITY_CATEGORY_CONFIG,
        )
        .extend(
            {
                cv.Optional(CONF_INITIAL_VALUE, default=DEFAULT_CUTOFF): cv.float_range(
                    min=MIN_CUTOFF, max=MAX_CUTOFF
                ),
                cv.Optional(CONF_MIN_VALUE, default=MIN_CUTOFF): cv.float_,
                cv.Optional(CONF_MAX_VALUE, default=MAX_CUTOFF): cv.float_,
                cv.Optional(CONF_STEP, default=DEFAULT_STEP): cv.positive_float,
                cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RTSP_AUDIO_ID])
    if conf := config.get(CONF_LOWCUT_FILTER_FREQUENCY):
        var = await number.new_number(
            conf,
            min_value=conf[CONF_MIN_VALUE],
            max_value=conf[CONF_MAX_VALUE],
            step=conf[CONF_STEP],
        )
        await cg.register_component(var, conf)
        cg.add(var.set_parent(parent))
        cg.add(var.set_initial_value(conf[CONF_INITIAL_VALUE]))
        cg.add(var.set_restore_value(conf[CONF_RESTORE_VALUE]))

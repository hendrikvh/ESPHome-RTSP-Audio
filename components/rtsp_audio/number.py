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
    UNIT_DECIBEL,
    UNIT_HERTZ,
)

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent, rtsp_audio_ns

DEPENDENCIES = ["rtsp_audio"]

CONF_LOWCUT_FILTER_FREQUENCY = "lowcut_filter_frequency"
CONF_HIGHCUT_FILTER_FREQUENCY = "highcut_filter_frequency"
CONF_GAIN_DB = "gain_db"

# Mirrors dc_blocker.h: DC_BLOCKER_DEFAULT_CUTOFF_HZ, MIN_CUTOFF_HZ,
# MAX_CUTOFF_HZ. The C++ side also clamps internally, but having the
# bounds here means HA only ever sees valid values on the slider.
DEFAULT_CUTOFF = 100.0
MIN_CUTOFF = 20.0
MAX_CUTOFF = 500.0
DEFAULT_STEP = 10.0

# Mirrors high_cut.h: HIGH_CUT_DEFAULT_CUTOFF_HZ, MIN_CUTOFF_HZ,
# MAX_CUTOFF_HZ. The default is the max (20 kHz), which the C++ side
# treats as "filter off" via a sentinel coefficient — bit-identical to a
# build without this stage.
DEFAULT_HIGHCUT = 20000.0
MIN_HIGHCUT = 1000.0
MAX_HIGHCUT = 20000.0
DEFAULT_HIGHCUT_STEP = 100.0

# Mirrors gain.h: GAIN_DB_DEFAULT, GAIN_DB_MIN, GAIN_DB_MAX. The slider
# is in dB; 0 dB is unity and lands exactly on the bit-identical Q8 fast
# path in the RTP loop. 1 dB step matches the ~audibility threshold.
DEFAULT_GAIN_DB = 0.0
MIN_GAIN_DB = -20.0
MAX_GAIN_DB = 40.0
DEFAULT_GAIN_DB_STEP = 1.0

RtspAudioLowCutFilterNumber = rtsp_audio_ns.class_(
    "RtspAudioLowCutFilterNumber", number.Number, cg.Component
)
RtspAudioHighCutFilterNumber = rtsp_audio_ns.class_(
    "RtspAudioHighCutFilterNumber", number.Number, cg.Component
)
RtspAudioGainDbNumber = rtsp_audio_ns.class_(
    "RtspAudioGainDbNumber", number.Number, cg.Component
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
        cv.Optional(CONF_HIGHCUT_FILTER_FREQUENCY): number.number_schema(
            RtspAudioHighCutFilterNumber,
            unit_of_measurement=UNIT_HERTZ,
            entity_category=ENTITY_CATEGORY_CONFIG,
        )
        .extend(
            {
                cv.Optional(
                    CONF_INITIAL_VALUE, default=DEFAULT_HIGHCUT
                ): cv.float_range(min=MIN_HIGHCUT, max=MAX_HIGHCUT),
                cv.Optional(CONF_MIN_VALUE, default=MIN_HIGHCUT): cv.float_,
                cv.Optional(CONF_MAX_VALUE, default=MAX_HIGHCUT): cv.float_,
                cv.Optional(CONF_STEP, default=DEFAULT_HIGHCUT_STEP): cv.positive_float,
                cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
            }
        )
        .extend(cv.COMPONENT_SCHEMA),
        cv.Optional(CONF_GAIN_DB): number.number_schema(
            RtspAudioGainDbNumber,
            unit_of_measurement=UNIT_DECIBEL,
            entity_category=ENTITY_CATEGORY_CONFIG,
        )
        .extend(
            {
                cv.Optional(
                    CONF_INITIAL_VALUE, default=DEFAULT_GAIN_DB
                ): cv.float_range(min=MIN_GAIN_DB, max=MAX_GAIN_DB),
                cv.Optional(CONF_MIN_VALUE, default=MIN_GAIN_DB): cv.float_,
                cv.Optional(CONF_MAX_VALUE, default=MAX_GAIN_DB): cv.float_,
                cv.Optional(CONF_STEP, default=DEFAULT_GAIN_DB_STEP): cv.positive_float,
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
    if conf := config.get(CONF_HIGHCUT_FILTER_FREQUENCY):
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
    if conf := config.get(CONF_GAIN_DB):
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

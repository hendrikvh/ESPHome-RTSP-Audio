import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_BYTES,
    UNIT_DECIBEL,
    UNIT_PERCENT,
)

from . import CONF_RTSP_AUDIO_ID, RtspAudioComponent

DEPENDENCIES = ["rtsp_audio"]

CONF_BYTES_SENT = "bytes_sent"
CONF_CPU_USE_PCT = "cpu_use_pct"
CONF_PEAK_LEVEL_DBFS = "peak_level_dbfs"
CONF_LIMITER_GAIN_REDUCTION_DB = "limiter_gain_reduction_db"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_RTSP_AUDIO_ID): cv.use_id(RtspAudioComponent),
        cv.Optional(CONF_BYTES_SENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_BYTES,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CPU_USE_PCT): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PEAK_LEVEL_DBFS): sensor.sensor_schema(
            # Defaulting the unit string to "dBFS" (rather than "dB" via
            # UNIT_DECIBEL) so users don't have to override it in every
            # example YAML to get the correct full-scale-reference label
            # showing in HA. ESPHome doesn't ship a UNIT_DECIBELS_FULL_SCALE
            # constant, so a string literal is the canonical workaround.
            unit_of_measurement="dBFS",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LIMITER_GAIN_REDUCTION_DB): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RTSP_AUDIO_ID])
    if conf := config.get(CONF_BYTES_SENT):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_bytes_sent_sensor(sens))
    if conf := config.get(CONF_CPU_USE_PCT):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_cpu_use_pct_sensor(sens))
    if conf := config.get(CONF_PEAK_LEVEL_DBFS):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_peak_level_dbfs_sensor(sens))
    if conf := config.get(CONF_LIMITER_GAIN_REDUCTION_DB):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_limiter_gain_reduction_db_sensor(sens))

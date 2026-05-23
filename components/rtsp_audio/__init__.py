import esphome.codegen as cg
from esphome.components import microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_PORT, Framework

CONF_PACKET_MS = "packet_ms"

CODEOWNERS = ["@hendrikvh"]
AUTO_LOAD = ["socket", "network"]
DEPENDENCIES = ["microphone"]

rtsp_audio_ns = cg.esphome_ns.namespace("rtsp_audio")
RtspAudioComponent = rtsp_audio_ns.class_("RtspAudioComponent", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RtspAudioComponent),
            cv.Optional(CONF_PORT, default=554): cv.port,
            cv.Optional(CONF_PACKET_MS, default=20): cv.int_range(10, 100),
            cv.Optional(
                CONF_MICROPHONE, default={}
            ): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework(Framework.ESP_IDF),
)

FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Optional(
            CONF_MICROPHONE
        ): microphone.final_validate_microphone_source_schema(
            "rtsp_audio", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_listen_port(config[CONF_PORT]))
    cg.add(var.set_packet_duration_ms(config[CONF_PACKET_MS]))
    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))
    cg.add_define("USE_RTSP_AUDIO")

"""Declarative layer — sensor telemetry.

Two distinct abstractions:

1. espnow_publish mixin (sender side)
   Attached to an existing sensor by adding publish_sensors: under espnow:.
   On every sensor value update, encodes the float as 4 bytes (IEEE 754 LE)
   and sends to the configured peer.

   YAML:
       espnow:
         publish_sensors:
           - sensor_id: temp
             peer: hub

2. platform: espnow sensor (receiver side)
   A self-contained sensor entity that registers a receive handler.
   When a frame arrives from the configured source peer, decodes the
   4-byte float payload and calls publish_state().

   YAML:
       sensor:
         - platform: espnow
           id: remote_temp
           source: sensor_node
           name: "Remote Temperature"
           unit_of_measurement: "°C"
           accuracy_decimals: 1
           device_class: temperature

Both sides use the same float encoding, making pairing automatic.
C++ classes: ESPNowSensor and SensorPublishHandler in espnow_component.h (USE_SENSOR guard).
"""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import HexInt

from . import (
    ESPNowComponent,
    _PEER_MAP,
    _mac_to_array,
    _validate_peer_ref,
    espnow_ns,
    peer_address_t,
)

CONF_SENSOR_ID = "sensor_id"
CONF_SOURCE = "source"
CONF_ESPNOW_ID = "espnow_id"

ESPNowSensor = espnow_ns.class_("ESPNowSensor", sensor.Sensor, cg.Component)
SensorPublishHandler = espnow_ns.class_("SensorPublishHandler")

# ── publish_sensors (sender mixin) ────────────────────────────────────────────

PUBLISH_SENSORS_SCHEMA = cv.ensure_list(
    cv.Schema(
        {
            cv.Required(CONF_SENSOR_ID): cv.use_id(sensor.Sensor),
            cv.Required("peer"): _validate_peer_ref,
        }
    )
)


def _resolve_peer_ref(ref) -> list:
    if isinstance(ref, str) and ref in _PEER_MAP:
        return list(_PEER_MAP[ref])
    if hasattr(ref, "parts"):
        return list(ref.parts)
    raise cv.Invalid(f"Unknown peer reference: {ref!r}")


async def publish_sensors_to_code(espnow_var, publish_list):
    for entry in publish_list:
        sensor_var = await cg.get_variable(entry[CONF_SENSOR_ID])
        parts = _resolve_peer_ref(entry["peer"])
        mac_arr = _mac_to_array(parts)

        handler = cg.new_Pvariable(
            cg.new_id(SensorPublishHandler), SensorPublishHandler, espnow_var, mac_arr
        )
        # Wire: sensor_var->add_on_state_callback([handler](float v) { handler->on_value(v); })
        # Using a LambdaAction-equivalent: ESPHome sensor's add_on_state_callback accepts
        # std::function<void(float)>. We bind the handler's method via a capturing lambda
        # in the generated C++ (generated lambdas are acceptable; YAML lambdas are not).
        cg.add(
            cg.RawExpression(
                f"{sensor_var}->add_on_state_callback("
                f"[{handler}](float v) {{ {handler}->on_value(v); }})"
            )
        )


# ── platform: espnow sensor (receiver) ───────────────────────────────────────

CONFIG_SCHEMA = (
    sensor.sensor_schema(ESPNowSensor)
    .extend(
        {
            cv.GenerateID(CONF_ESPNOW_ID): cv.use_id(ESPNowComponent),
            cv.Required(CONF_SOURCE): _validate_peer_ref,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    source = config[CONF_SOURCE]
    parts = _resolve_peer_ref(source)
    mac_arr = _mac_to_array(parts)

    espnow_var = await cg.get_variable(config[CONF_ESPNOW_ID])

    var = cg.new_Pvariable(config[CONF_ID], ESPNowSensor, espnow_var, mac_arr)
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)

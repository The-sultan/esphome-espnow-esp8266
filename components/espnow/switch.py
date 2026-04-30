"""Declarative layer — switch sync helper.

Provides sync_switches_to_code(), called by __init__.py's to_code when the
espnow: sync_switches: key is present.

YAML example (under espnow:, not under switch:):
    espnow:
      channel: 0
      peers:
        - mac: "11:22:33:44:55:66"
          id: node_b
      sync_switches:
        - switch_id: relay
          peers: [node_b]

    switch:
      - platform: gpio
        pin: GPIO12
        id: relay

This expands to (design doc §6.2 case 1):
  - on turn_on/turn_off: send new state to all listed peers
  - on on_receive from any listed peer: apply state locally without re-transmitting

The C++ implementation uses SwitchSyncGroup from espnow_component.h (USE_SWITCH guard).
"""

import esphome.codegen as cg
from esphome.components import switch as switch_component
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

CONF_SWITCH_ID = "switch_id"
CONF_SYNC_PEERS = "peers"

SwitchSyncGroup = espnow_ns.class_("SwitchSyncGroup")

SYNC_SWITCHES_SCHEMA = cv.ensure_list(
    cv.Schema(
        {
            cv.Required(CONF_SWITCH_ID): cv.use_id(switch_component.Switch),
            cv.Required(CONF_SYNC_PEERS): cv.ensure_list(_validate_peer_ref),
        }
    )
)


def _resolve_peer_ref(ref) -> list:
    """Return 6-element list of ints for a peer id string or MACAddress."""
    import esphome.core as ecore

    if isinstance(ref, str) and ref in _PEER_MAP:
        return list(_PEER_MAP[ref])
    if hasattr(ref, "parts"):
        return list(ref.parts)
    raise cv.Invalid(f"Unknown peer reference: {ref!r}")


async def sync_switches_to_code(espnow_var, sync_list):
    for entry in sync_list:
        switch_var = await cg.get_variable(entry[CONF_SWITCH_ID])

        group = cg.new_Pvariable(
            cg.new_id(SwitchSyncGroup), SwitchSyncGroup, espnow_var, switch_var
        )

        for ref in entry[CONF_SYNC_PEERS]:
            parts = _resolve_peer_ref(ref)
            mac_arr = _mac_to_array(parts)
            cg.add(group.add_peer(mac_arr))

        cg.add(espnow_var.register_receive_handler(group))

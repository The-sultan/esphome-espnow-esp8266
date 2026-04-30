from esphome import automation, core
import esphome.codegen as cg
from esphome.components import wifi
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_CHANNEL,
    CONF_DATA,
    CONF_ID,
    CONF_TRIGGER_ID,
)
from esphome.core import HexInt

CODEOWNERS = ["@farid-elias"]
DEPENDENCIES = []

# ── Namespace / class references ──────────────────────────────────────────────

espnow_ns = cg.esphome_ns.namespace("espnow")
ESPNowComponent = espnow_ns.class_("ESPNowComponent", cg.Component)

ESPNowRecvInfo = espnow_ns.class_("ESPNowRecvInfo")
ESPNowRecvInfoConstRef = ESPNowRecvInfo.operator("const").operator("ref")

ESPNowReceivePacketHandler = espnow_ns.class_("ESPNowReceivePacketHandler")
ESPNowBroadcastHandler = espnow_ns.class_("ESPNowBroadcastHandler")
ESPNowUnknownPeerHandler = espnow_ns.class_("ESPNowUnknownPeerHandler")

byte_vector = cg.std_vector.template(cg.uint8)
peer_address_t = cg.std_ns.class_("array").template(cg.uint8, 6)

SendAction = espnow_ns.class_("SendAction", automation.Action)
AddPeerAction = espnow_ns.class_("AddPeerAction", automation.Action)
DeletePeerAction = espnow_ns.class_("DeletePeerAction", automation.Action)

ESPNowHandlerTrigger = automation.Trigger.template(
    ESPNowRecvInfoConstRef,
    cg.uint8.operator("const").operator("ptr"),
    cg.uint8,
)

OnReceiveTrigger = espnow_ns.class_(
    "OnReceiveTrigger", ESPNowHandlerTrigger, ESPNowReceivePacketHandler
)
OnBroadcastTrigger = espnow_ns.class_(
    "OnBroadcastTrigger", ESPNowHandlerTrigger, ESPNowBroadcastHandler
)
OnUnknownPeerTrigger = espnow_ns.class_(
    "OnUnknownPeerTrigger", ESPNowHandlerTrigger, ESPNowUnknownPeerHandler
)

# ── Constants ─────────────────────────────────────────────────────────────────

CONF_AUTO_ADD_PEER = "auto_add_peer"
CONF_PEERS = "peers"
CONF_MAC = "mac"
CONF_LMK = "lmk"
CONF_PMK = "pmk"
CONF_ON_RECEIVE = "on_receive"
CONF_ON_BROADCAST = "on_broadcast"
CONF_ON_UNKNOWN_PEER = "on_unknown_peer"
CONF_WAIT_FOR_SENT = "wait_for_sent"
CONF_CONTINUE_ON_ERROR = "continue_on_error"

MAX_ESPNOW_PACKET_SIZE = 250
MAX_LMK_PMK_SIZE = 16

# Module-level peer-ID map, populated in to_code() before action handlers run.
# Maps peer id string → tuple of 6 MAC bytes.
_PEER_MAP: dict = {}

# ── Validators ────────────────────────────────────────────────────────────────


def _validate_raw_key(value):
    """Validate a 16-byte key (PMK or LMK): list of hex bytes or a 16-char string."""
    if isinstance(value, str):
        if len(value) != MAX_LMK_PMK_SIZE:
            raise cv.Invalid(
                f"Key string must be exactly {MAX_LMK_PMK_SIZE} characters, got {len(value)}"
            )
        return [ord(c) for c in value]
    if isinstance(value, list):
        if len(value) != MAX_LMK_PMK_SIZE:
            raise cv.Invalid(
                f"Key must be exactly {MAX_LMK_PMK_SIZE} bytes, got {len(value)}"
            )
        return cv.Schema([cv.hex_uint8_t])(value)
    raise cv.Invalid("Key must be a 16-character string or a list of 16 hex bytes")


def _validate_payload(value):
    if isinstance(value, str):
        if len(value) > MAX_ESPNOW_PACKET_SIZE:
            raise cv.Invalid(
                f"Payload string must be at most {MAX_ESPNOW_PACKET_SIZE} characters"
            )
        return value
    if isinstance(value, list):
        if len(value) > MAX_ESPNOW_PACKET_SIZE:
            raise cv.Invalid(
                f"Payload must be at most {MAX_ESPNOW_PACKET_SIZE} bytes"
            )
        return cv.Schema([cv.hex_uint8_t])(value)
    raise cv.Invalid("Payload must be a string or a list of hex bytes")


def _validate_channel(value):
    if value is None:
        raise cv.Invalid("channel is required when wifi: is not configured")
    return wifi.validate_channel(value)


def _validate_peer_ref(value):
    """Accept a MAC address string or a peer id string (resolved at codegen time)."""
    if isinstance(value, cv.Lambda):
        return cv.returning_lambda(value)
    try:
        return cv.mac_address(value)
    except cv.Invalid:
        return cv.string_strict(value)


# ── Peer schema ───────────────────────────────────────────────────────────────

_PEER_FULL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MAC): cv.mac_address,
        cv.Optional(CONF_ID): cv.string_strict,
        cv.Optional(CONF_CHANNEL, default=0): cv.int_range(min=0, max=13),
        cv.Optional(CONF_LMK): _validate_raw_key,
    }
)

_PEER_SCHEMA = cv.maybe_simple_value(_PEER_FULL_SCHEMA, key=CONF_MAC)

# ── Component schema ──────────────────────────────────────────────────────────

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ESPNowComponent),
            cv.OnlyWithout(CONF_CHANNEL, "wifi"): _validate_channel,
            cv.Optional(CONF_AUTO_ADD_PEER, default=False): cv.boolean,
            cv.Optional(CONF_PMK): _validate_raw_key,
            cv.Optional(CONF_PEERS): cv.ensure_list(_PEER_SCHEMA),
            cv.Optional(CONF_ON_RECEIVE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnReceiveTrigger),
                    cv.Optional(CONF_ADDRESS): cv.mac_address,
                }
            ),
            cv.Optional(CONF_ON_BROADCAST): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnBroadcastTrigger),
                    cv.Optional(CONF_ADDRESS): cv.mac_address,
                }
            ),
            cv.Optional(CONF_ON_UNKNOWN_PEER): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnUnknownPeerTrigger),
                },
                single=True,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp8266,
)

# ── Codegen helpers ───────────────────────────────────────────────────────────


def _mac_to_array(mac_parts):
    return cg.ArrayInitializer(*[HexInt(b) for b in mac_parts])


async def _trigger_to_code(config, trigger_id_key):
    args = []
    if address := config.get(CONF_ADDRESS):
        args.append(_mac_to_array(address.parts))
    return cg.new_Pvariable(config[trigger_id_key], *args)


async def _register_peer(var, config, args):
    """Resolve peer address (MAC or id string) and call set_address on var."""
    peer = config[CONF_ADDRESS]
    if isinstance(peer, str):
        # Peer id — look up in map (populated at start of to_code)
        if peer not in _PEER_MAP:
            raise cv.Invalid(
                f"Unknown peer id '{peer}'. Make sure it is declared under espnow: peers:"
            )
        parts = _PEER_MAP[peer]
        peer = [HexInt(p) for p in parts]
    elif isinstance(peer, core.MACAddress):
        peer = [HexInt(p) for p in peer.parts]

    template_ = await cg.templatable(peer, args, peer_address_t, peer_address_t)
    cg.add(var.set_address(template_))


# ── to_code ───────────────────────────────────────────────────────────────────


async def to_code(config):
    global _PEER_MAP
    _PEER_MAP = {}

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_ESPNOW")

    # Populate peer id map before any action handlers run
    for peer in config.get(CONF_PEERS, []):
        if peer_id := peer.get(CONF_ID):
            _PEER_MAP[peer_id] = peer[CONF_MAC].parts

    if wifi_channel := config.get(CONF_CHANNEL):
        cg.add(var.set_channel(wifi_channel))

    cg.add(var.set_auto_add_peer(config[CONF_AUTO_ADD_PEER]))

    if pmk := config.get(CONF_PMK):
        cg.add(var.set_pmk(cg.ArrayInitializer(*[HexInt(b) for b in pmk])))

    for peer in config.get(CONF_PEERS, []):
        mac_arr = _mac_to_array(peer[CONF_MAC].parts)
        channel = peer.get(CONF_CHANNEL, 0)
        if lmk := peer.get(CONF_LMK):
            lmk_arr = cg.ArrayInitializer(*[HexInt(b) for b in lmk])
            cg.add(var.add_static_peer_with_lmk(mac_arr, channel, lmk_arr))
        else:
            cg.add(var.add_static_peer(mac_arr, channel))

    for on_receive in config.get(CONF_ON_RECEIVE, []):
        trigger = await _trigger_to_code(on_receive, CONF_TRIGGER_ID)
        await automation.build_automation(
            trigger,
            [
                (ESPNowRecvInfoConstRef, "info"),
                (cg.uint8.operator("const").operator("ptr"), "data"),
                (cg.uint8, "size"),
            ],
            on_receive,
        )
        cg.add(var.register_receive_handler(trigger))

    for on_broadcast in config.get(CONF_ON_BROADCAST, []):
        trigger = await _trigger_to_code(on_broadcast, CONF_TRIGGER_ID)
        await automation.build_automation(
            trigger,
            [
                (ESPNowRecvInfoConstRef, "info"),
                (cg.uint8.operator("const").operator("ptr"), "data"),
                (cg.uint8, "size"),
            ],
            on_broadcast,
        )
        cg.add(var.register_broadcast_handler(trigger))

    if on_unknown := config.get(CONF_ON_UNKNOWN_PEER):
        trigger = await _trigger_to_code(on_unknown, CONF_TRIGGER_ID)
        await automation.build_automation(
            trigger,
            [
                (ESPNowRecvInfoConstRef, "info"),
                (cg.uint8.operator("const").operator("ptr"), "data"),
                (cg.uint8, "size"),
            ],
            on_unknown,
        )
        cg.add(var.register_unknown_peer_handler(trigger))


# ── Actions ───────────────────────────────────────────────────────────────────

_PEER_REF_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(ESPNowComponent),
        cv.Required(CONF_ADDRESS): cv.templatable(_validate_peer_ref),
    }
)

_SEND_SCHEMA = _PEER_REF_SCHEMA.extend(
    {
        cv.Required(CONF_DATA): cv.templatable(_validate_payload),
        cv.Optional(CONF_WAIT_FOR_SENT, default=True): cv.boolean,
        cv.Optional(CONF_CONTINUE_ON_ERROR, default=True): cv.boolean,
    }
)


def _validate_send_schema(config):
    if not config[CONF_WAIT_FOR_SENT] and not config[CONF_CONTINUE_ON_ERROR]:
        raise cv.Invalid(
            f"'{CONF_CONTINUE_ON_ERROR}' cannot be false when '{CONF_WAIT_FOR_SENT}' is false "
            "— the automation will not wait for the failed result.",
            path=[CONF_CONTINUE_ON_ERROR],
        )
    return config


_SEND_SCHEMA.add_extra(_validate_send_schema)


@automation.register_action(
    "espnow.send",
    SendAction,
    _SEND_SCHEMA,
    synchronous=False,
)
@automation.register_action(
    "espnow.broadcast",
    SendAction,
    cv.maybe_simple_value(
        _SEND_SCHEMA.extend(
            {
                cv.Optional(CONF_ADDRESS, default="FF:FF:FF:FF:FF:FF"): cv.mac_address,
            }
        ),
        key=CONF_DATA,
    ),
    synchronous=False,
)
async def send_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])

    await _register_peer(var, config, args)

    data = config.get(CONF_DATA, [])
    if isinstance(data, str):
        data = list(data.encode())
    templ = await cg.templatable(data, args, byte_vector, byte_vector)
    cg.add(var.set_data(templ))

    cg.add(var.set_wait_for_sent(config[CONF_WAIT_FOR_SENT]))
    cg.add(var.set_continue_on_error(config[CONF_CONTINUE_ON_ERROR]))

    return var


@automation.register_action(
    "espnow.peer.add",
    AddPeerAction,
    cv.maybe_simple_value(_PEER_REF_SCHEMA, key=CONF_ADDRESS),
    synchronous=True,
)
@automation.register_action(
    "espnow.peer.delete",
    DeletePeerAction,
    cv.maybe_simple_value(_PEER_REF_SCHEMA, key=CONF_ADDRESS),
    synchronous=True,
)
async def peer_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    await _register_peer(var, config, args)
    return var

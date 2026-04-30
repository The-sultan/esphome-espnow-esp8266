# ESP-NOW External Component for ESP8266 — Design Document

**Version:** 1.0  
**Status:** v1.0 — ready for implementation

---

## Contents

1. [Context and Motivation](#1-context-and-motivation)
2. [Technical Background on ESP-NOW](#2-technical-background-on-esp-now)
3. [Target Use Cases](#3-target-use-cases)
4. [Design Principles](#4-design-principles)
5. [Proposed Architecture](#5-proposed-architecture)
6. [YAML API Sketches](#6-yaml-api-sketches)
7. [ESP8266-Specific Considerations](#7-esp8266-specific-considerations)
8. [Open Questions](#8-open-questions)
9. [Out of Scope for v1](#9-out-of-scope-for-v1)
10. [References](#10-references)

---

## 1. Context and Motivation

ESPHome is the dominant declarative firmware platform for ESP-based devices in self-hosted home automation. It covers the full stack from sensor reading to WiFi connectivity to Home Assistant integration, with a YAML-driven model that lets experienced users configure complex behavior without writing firmware code.

ESP8266 and ESP8285 remain widely deployed. A large fraction of the Sonoff catalog — Basic, TX series, Mini — runs on these chips, and they continue to ship in new products. Despite ESPHome introducing official ESP-NOW support in the 2025.8 release, that implementation is ESP32-only. The chip difference is not incidental: the official component is built on top of the ESP-IDF ESP-NOW API, which does not exist on ESP8266.

Community external components for ESP8266 ESP-NOW do exist. The ones found at the time of writing share a common set of problems: they require lambdas in user YAML, include manual `.h` file imports, or are structured as `custom_component` — a mechanism that was deprecated in ESPHome 2024.x and removed in 2025.2. None of them would pass review as a first-party ESPHome component, and none provide the layered API model described in this document.

**Minimum supported ESPHome version: 2025.2.** This is the release that removed `custom_component` and formalized `external_components` as the only supported mechanism for out-of-tree components. This component does not use any ESPHome feature introduced after 2025.2; the version floor is set by the `custom_component` removal, not by any API dependency.

The motivating use case is a pair of Sonoff TX switches (ESP8285) controlling the same light from two physical locations. Today they are synchronized through Home Assistant: switch A triggers an automation that commands switch B. This introduces a perceptible round-trip latency — WiFi → HA automation → WiFi — that is noticeable when someone is at the wall. ESP-NOW eliminates the HA dependency for this path entirely. Both devices communicate directly at the 802.11 data link layer; the synchronization path involves no router, no broker, and no cloud.

That said, the motivating use case is two devices. The protocol supports up to 20 peers. This component is designed against the protocol's actual capability, not against the use case that motivated its creation.

---

## 2. Technical Background on ESP-NOW

### 2.1 Protocol Layer

ESP-NOW is an Espressif proprietary protocol that operates at IEEE 802.11 layer 2, using vendor-specific action frames.[^1] There is no WiFi association involved: devices do not need to join an access point to communicate with each other, and the protocol works regardless of whether a WiFi connection to an AP is present.

The frame structure is:

```
MAC Header (24 B)
  ├── Destination address
  ├── Source address
  └── BSSID: FF:FF:FF:FF:FF:FF (broadcast)
Category Code (1 B): 0x7F (vendor-specific)
Organization Identifier (3 B): 0x18 0xFE 0x34 (Espressif)
Random value (4 B)
Vendor-specific element:
  ├── Element ID
  ├── Length
  ├── Organization ID
  ├── Type
  ├── Version
  └── Body (0–250 B)
FCS (4 B)
```

Note that the FromDS and ToDS bits in the MAC header are both 0, and the third address field is set to the broadcast address regardless of whether the frame is unicast or multicast.[^1] This distinguishes ESP-NOW frames from normal data frames at the 802.11 level.

The default PHY bitrate is 1 Mbps.

### 2.2 Payload and Peer Limits

The maximum application payload is **250 bytes** per frame (v1.0 protocol, `ESP_NOW_MAX_DATA_LEN`). ESP-IDF introduced ESP-NOW v2.0 for ESP32 with a 1470-byte limit, but ESP8266 implements v1.0 only. The 250-byte constraint applies to all ESP8266 deployments without exception.

Peer limits for ESP8266:[^2]

- Maximum total peers: **20** (`ESP_NOW_MAX_TOTAL_PEER_NUM`)
- Maximum encrypted peers: **6** (`ESP_NOW_MAX_ENCRYPT_PEER_NUM`)

On ESP32 with IDF, the encrypted peer limit is configurable and defaults to 7 (up to 17). On ESP8266 the limit is fixed at 6 with no runtime configuration.

### 2.3 Encryption

ESP-NOW uses a two-level key hierarchy:[^1]

**PMK (Primary Master Key):** 16 bytes. Set per node with `esp_now_set_pmk()`. Used to encrypt the LMKs. If no PMK is set, a fixed default is used — this provides no meaningful security.

**LMK (Local Master Key):** 16 bytes, set per peer. Used to encrypt the vendor-specific action frame body using CCMP (Counter Mode with CBC-MAC Protocol, AES-128, as defined in IEEE 802.11-2012).

Broadcast and multicast frames cannot be encrypted. Only unicast frames to peers that have an LMK configured are encrypted.

Encryption is optional on a per-peer basis. A node can have some encrypted peers and some unencrypted ones simultaneously, subject to the 6-peer encrypted limit.

### 2.4 Channel Handling

All devices communicating via ESP-NOW must be on the same 802.11 channel. When adding a peer, the channel field of `esp_now_peer_info_t` accepts either an explicit channel (1–13) or `0`, which means "use the current channel of the active WiFi interface."[^2]

Channel 0 is the correct choice in the common case where the device also runs a `wifi:` component in STA mode: the channel is then determined by the AP the device is associated to, and ESP-NOW frames are sent and received on that same channel without any extra configuration.

When no `wifi:` component is active (ESP-NOW-only mode), the channel must be set explicitly. All communicating nodes must agree on a channel out of band (typically hardcoded in the YAML).

The constraint that all peers share one channel becomes important in environments where different APs are used on different channels. In a typical home installation with a single SSID, this is not a problem.

### 2.5 Callback Model

The ESP8266 RTOS SDK exposes two callbacks:[^2]

**Send callback** (`esp_now_send_cb_t`): called after the frame has been transmitted, with the destination MAC and a status (`ESP_NOW_SEND_SUCCESS` / `ESP_NOW_SEND_FAIL`). The status reflects MAC-layer acknowledgment — it confirms the frame left the radio and was ACKed at layer 2. It does not confirm delivery to the remote application.

**Receive callback** (`esp_now_recv_cb_t`): called when a valid ESP-NOW frame is received, with the source MAC address, payload pointer, and payload length.

On ESP8266, the receive callback signature is:

```c
void on_recv(const uint8_t *mac_addr, const uint8_t *data, int data_len);
```

This is notably different from the current ESP32 IDF version, which passes an `esp_now_recv_info_t` struct containing source address, destination address, RSSI, and timestamp. **On ESP8266, RSSI and destination MAC are not available in the receive callback.** This is a hard platform constraint, not a design choice.

Both callbacks execute in the WiFi task context, which has higher priority than the main ESPHome loop. All non-trivial work must be deferred from callback context to the main loop.

### 2.6 ESP8266 API vs ESP32 ESP-IDF

The ESP8266 RTOS SDK exposes an IDF-style API (`esp8266/include/esp_now.h`) that is largely compatible with the ESP32 IDF at the function-name level, but with the following differences:[^2][^3]

| Feature | ESP8266 | ESP32 IDF |
|---|---|---|
| Encrypted peer limit | 6 (fixed) | 7 default, up to 17 configurable |
| ESP-NOW v2.0 (1470 B payload) | Not supported | Supported |
| Recv callback includes RSSI/dest | No | Yes (newer IDF) |
| PHY rate config per peer | No | Yes (`esp_now_rate_config_t`) |
| Wake window (power management) | No | Yes |
| Role system (CONTROLLER/SLAVE) | Old NonOS SDK only | Never existed in IDF |

The "role" system (`ESP_NOW_ROLE_CONTROLLER`, `ESP_NOW_ROLE_SLAVE`, `ESP_NOW_ROLE_COMBO`) belongs to the legacy NonOS SDK (pre-RTOS). It does not exist in the RTOS SDK or in the IDF. Any reference to roles in community ESP8266 code is a sign of an outdated SDK dependency.

---

## 3. Target Use Cases

### 3.1 v1 Use Cases

**Case 1: Shared state synchronization between N peers.**  
Multiple nodes share ownership of a single logical state (e.g., a light's on/off state). Any node can change the state; all nodes must reflect the current state. The model is symmetric: no node has a privileged role. When a node changes its local state, it broadcasts the new value to all peers; when a node receives a state update, it applies it locally.

The motivating instance of this case is two Sonoff TX switches controlling the same light. One is wired to the relay; the other is wired only to mains (the relay output is unconnected). From the component's perspective, both are symmetric participants in a state sync group — the electrical asymmetry is invisible.

Conflict resolution: last-write-wins, implicitly. The probability of two users pressing physical buttons simultaneously is negligible for residential use, so no additional mechanism is warranted.

**Case 2: Unidirectional telemetry from sensor node to hub.**  
A battery- or remotely-placed sensor node (temperature, humidity, motion) periodically sends readings to a hub node. The hub receives the data and exposes it to ESPHome's sensor infrastructure (and thus to Home Assistant). This is a strict sender-receiver relationship with no response.

### 3.2 Future Use Cases

These use cases are out of scope for v1 but are explicitly considered in the primitive design. The v1 primitives must be sufficient to implement each of these without firmware modification.

**Case 3: Unidirectional remote command (actuator side).**  
One node sends a command; another node executes it. Conceptually the inverse of case 2: instead of reporting a sensor reading, the sender triggers an action on the receiver. This is constructible from v1 primitives (`espnow.send` to the target peer; `on_receive` on the receiver to execute the desired action). A declarative abstraction for this case is deferred.

**Case 4: Broadcast / topic-based pub-sub without explicit peer list.**  
A node publishes to a topic without maintaining a per-peer list; any node subscribed to that topic receives it. This requires either a broadcast send or a group MAC. The `espnow.broadcast` action covers the send side; a higher-level pub-sub abstraction that manages subscriptions and routing is deferred.

**Case 5: Mesh repeater / range extender.**  
A node relays frames on behalf of others to extend the physical range of the network. This requires topology awareness and forwarding logic that goes beyond the scope of a simple ESP-NOW component. Not blocked by primitives, but not a v1 priority.

---

## 4. Design Principles

**Layered API with both layers as first-class citizens.** The component exposes two public APIs. The flexible layer consists of primitives that mirror what ESP-NOW actually provides: peer management, send, receive, channel configuration. The declarative layer consists of higher-level abstractions — like state sync groups or telemetry channels — that are implemented as explicit, documented compositions of those primitives. Neither layer is secondary. Users can mix them freely in the same YAML, using declarative abstractions where they fit and dropping to primitives where they need more control. The key commitment: every declarative abstraction must be accompanied by documentation showing exactly which primitive operations it expands to.

**Primitives are designed against the protocol space, not against the motivating use case.** The motivating use case involves two switches. ESP-NOW supports up to 20 peers. The primitive for peer management handles N peers, not a "pair." This principle prevents the common trap of over-fitting an API to the first use case and then discovering it needs to be redesigned when a slightly different use case arrives. The motivating use case determines which primitives to prioritize and the shape of the declarative abstractions; it does not constrain the primitive surface.

**Minimal logic in firmware.** Any logic that can reasonably live in a higher layer — ESPHome automations, Home Assistant scripts, Node-RED — should live there. The firmware's responsibility is to provide reliable, low-latency transport. The exception to this principle is when delegating to a higher layer introduces an unacceptable cost, typically latency. Synchronizing two physical switches via Home Assistant costs a perceptible round trip. Synchronizing them via ESP-NOW over direct Layer 2 communication does not. That latency difference is the entire reason this component exists. Outside of cases with a latency justification, the rule holds: features like heartbeat detection, re-sync on reconnect, and conflict resolution beyond last-write-wins are left for higher layers.

**Completeness of v1 primitives.** The four primitives described in Section 5 must be sufficient for a user to implement any reasonable higher-order feature without modifying firmware. Heartbeat, liveness detection, re-sync on boot, and conditional routing are all implementable today using the primitives plus ESPHome's existing automation system (`interval:`, `on_boot:`, `script:`, `binary_sensor: template:`, etc.). Section 6 demonstrates this explicitly. If a future use case arises that cannot be expressed as a composition of v1 primitives, that indicates a gap in the primitive design — not an argument for adding a new primitive. The correct response is to close the gap.

**Target audience: experienced ESPHome users.** This component is not intended for someone encountering ESPHome for the first time. The person who reaches for an external component implementing a low-level wireless protocol has already navigated firmware flashing, YAML configuration, device discovery, and Home Assistant integration. The API should not be diluted to accommodate beginners. Complexity that would confuse a novice but is entirely natural to an experienced user — like hex byte arrays, MAC address strings, or explicit channel configuration — is acceptable and appropriate.

**Declarative YAML at the high level.** The declarative layer must be expressible in pure YAML: no `lambda:` blocks, no manual `.h` includes, no `custom_component:`. This is both an aesthetic goal and a compatibility requirement — `custom_component` was removed in ESPHome 2025.2. The component must use the modern `external_components:` mechanism with proper `__init__.py` + platform `.py` + `.cpp`/`.h` structure. Lambda use in the flexible layer is expected and acceptable; it is the nature of that layer. The no-lambda constraint applies to the declarative layer only.

**API alignment with the official ESPHome ESP-NOW component.** Where applicable, the public YAML API mirrors the official ESPHome ESP-NOW component for ESP32. This minimizes cognitive overhead for users moving between platforms and aligns this component with established conventions in the ecosystem. A user who has read the official component's documentation should find familiar keys, action names, and trigger names here. Divergence is permitted only when ESP8266-specific constraints make alignment impossible (e.g., the absence of RSSI in the receive callback), or when extending the API beyond what the official component covers (e.g., the declarative layer). In both cases the divergence must be documented explicitly.

**Minimum viable scope.** Features are included in v1 only when required by a v1 use case, or when their absence would force a workaround that contradicts another design principle. Features that are "nice to have," that align with external conventions but solve no concrete problem, or that anticipate future use cases without current evidence, are deferred to later versions. The cost of removing a feature later — a breaking change — is much higher than the cost of adding one — an additive change. The burden of proof therefore falls on inclusion, not on omission. This principle can come into tension with the alignment principle: when they conflict, minimum scope wins unless the divergence creates concrete cognitive overhead or migration friction that outweighs the cost of inclusion.

### Documented Divergences from the Official Component

This subsection is a living reference. Each entry documents a known divergence from the official ESPHome ESP-NOW component for ESP32, with its justification. Divergences arising from platform constraints are distinguished from deliberate design choices.

**Peer `id:` alias (deliberate extension, v1).** This component allows peers to be referenced by an optional `id:` label in addition to their MAC address. The official component uses raw MAC addresses exclusively. The `id:` alias is optional — any place that accepts an id also accepts a raw MAC string. This is a pure ergonomic addition: ESPHome's `id:` convention is standard for all other entity types (sensors, switches, scripts), and hardcoding MAC addresses at multiple YAML locations is a maintenance anti-pattern in multi-node deployments. Users who prefer strict alignment with the official component can ignore `id:` entirely.

**Absence of RSSI and destination address in receive callback (platform constraint, v1).** On ESP8266, the receive callback does not provide RSSI or destination MAC. The `info` struct in trigger context contains only `src_addr`. This cannot be worked around in software — it is a limitation of the ESP8266 SDK.

**Topic-based routing (deliberate deferral, target v2).** The official component has no topic concept; routing is left to the user via raw payload inspection. This component defers a topic abstraction to v2. See Section 9.

---

## 5. Proposed Architecture

### 5.1 Component Structure

```
components/espnow/
  __init__.py          # espnow: block — node identity, peer list, PMK, channel,
                       # on_receive/on_broadcast/on_unknown_peer triggers,
                       # espnow.send, espnow.broadcast, espnow.peer.add/delete actions
  switch.py            # declarative layer for switches: implements espnow_sync,
                       # which expands to flexible-layer send + on_receive handlers
  sensor.py            # declarative layer for sensors: implements espnow_publish
                       # mixin (sender side) and platform: espnow sensor (receiver side)
  espnow_component.h   # ESPNowComponent class + Action/Trigger declarations
  espnow_component.cpp # runtime: setup, loop dispatch, WiFi-task callback context
```

The Python layer is responsible for config validation and C++ code generation. It does not contain runtime logic. The C++ layer contains all runtime behavior.

### 5.2 The Four Primitives

**Primitive 1 — Node identity and peer list.**  
Configured under the top-level `espnow:` key. Declares the node's PMK (optional), the channel policy (explicit channel or inherit from `wifi:`), and the static list of peers with their MAC addresses and optional LMKs. Also exposes one behavioral flag that aligns with the official component:[^4][^5]

`auto_add_peer: false` — when set to true, any peer the device receives from is automatically registered as an unencrypted peer, even if not in the static list. When false (the default), frames from unknown peers fire the `on_unknown_peer` trigger instead of being silently discarded.

This primitive has no runtime behavior beyond what this flag configures — it is setup-time configuration translated into `esp_now_init()`, `esp_now_set_pmk()`, and `esp_now_add_peer()` calls.

**Primitive 2 — Publish (send and broadcast) and peer management.**  
Four actions in this group: `espnow.send`, `espnow.broadcast`, `espnow.peer.add`, and `espnow.peer.delete`.

`espnow.send` sends a payload to a single peer identified by MAC address or peer id. The peer must have been declared in the static list or added at runtime via `espnow.peer.add`. The `payload` field accepts a byte array literal, a plain string (converted to UTF-8), or a templatable value (lambda returning `std::vector<uint8_t>`). Two additional options align with the official component:[^4][^5] `wait_for_sent: true` blocks the automation until the MAC-layer send callback fires; `continue_on_error: false` halts the automation if the send fails.

`espnow.broadcast` sends a payload to `FF:FF:FF:FF:FF:FF`. It takes the same `payload`, `wait_for_sent`, and `continue_on_error` fields as `espnow.send` but no `peer` field. Broadcast frames cannot be encrypted. This action does not require the broadcast address to be in the peer list.

`espnow.peer.add` and `espnow.peer.delete` manage the peer list at runtime, taking a MAC address. These enable use cases where peers are discovered dynamically rather than declared statically — for example, a hub that accepts connections from arbitrary sensor nodes.

**Primitive 3 — Subscribe (receive).**  
Three triggers, all aligning with the official component:[^4][^6]

`on_receive` fires for unicast frames addressed to this device, optionally filtered by source address (`address:` key, accepting a peer id or raw MAC). `on_broadcast` fires for broadcast frames (`FF:FF:FF:FF:FF:FF`), also optionally filtered by source address. `on_unknown_peer` fires when a frame arrives from a MAC not in the peer list — useful for logging, alerting, or implementing custom peer discovery logic.

All three triggers expose the same context variables: `info` (a struct containing `src_addr`), `data` (`const uint8_t*`), and `size` (`uint8_t`). This naming matches the official component.[^5][^6] On ESP8266, `info` contains only `src_addr` — RSSI and destination address are not available in the receive callback (see Section 2.5). This is a documented platform divergence, not a design choice.

Because the receive callback fires in the WiFi task, all incoming frames are queued internally and dispatched in the main loop before any trigger fires.

**Primitive 4 — Channel configuration.**  
Controls which 802.11 channel ESP-NOW operates on. Two modes: explicit (a fixed integer 1–13, used when no `wifi:` component is present or when channel isolation is required) and automatic (channel `0`, which tracks the current STA/SoftAP channel). When `wifi:` is active, channel `0` is the correct default. When operating standalone, an explicit channel must be set. The channel field is part of each peer's configuration in `esp_now_peer_info_t`, so the channel policy is per-peer — though in practice all peers should use the same channel policy.

### 5.3 C++ Runtime

`ESPNowComponent` extends `esphome::Component`. In `setup()` it initializes ESP-NOW, sets the PMK if configured, and registers all declared peers. In `loop()` it dispatches messages from the receive queue. The queue is a fixed-size ring buffer to avoid heap fragmentation — a design constraint driven by ESP8266's limited DRAM (see Section 7).

The receive callback (`static void recv_cb(...)`) is a static method that runs in the WiFi task. Its only job is to push the incoming frame into the queue. All dispatch logic runs in `loop()` on the main task.

`ESPNowSendAction<>`, `ESPNowBroadcastAction<>`, `ESPNowAddPeerAction<>`, `ESPNowDeletePeerAction<>`, and `ESPNowSetChannelAction<>` are `Action<>` templates instantiated by the codegen for the corresponding YAML actions.

`ESPNowOnReceiveTrigger`, `ESPNowOnBroadcastTrigger`, and `ESPNowOnUnknownPeerTrigger` are `Trigger<const ESPNowRecvInfo &, const uint8_t *, uint8_t>` specializations that fire from `loop()` when the receive queue contains a matching frame.

### 5.4 Declarative Layer as Explicit Composition

Each declarative abstraction in `switch.py` and `sensor.py` is implemented by emitting, at codegen time, the same C++ and automation graph that a user would write manually using the flexible layer. The design documentation for each abstraction must include a "this expands to" block showing the equivalent primitive YAML. This is not just documentation policy — it is the design's correctness criterion. If an abstraction cannot be expressed as primitive composition, the primitive layer has a gap.

---

## 6. YAML API Sketches

These are design sketches, not a final API specification. Names, structure, and exact semantics may change during implementation. They exist to validate that the primitive design is coherent and complete.

### 6.1 Flexible Layer

#### Case 1 — Shared state sync, explicit primitive composition

Both nodes run identical configuration. Node A's MAC is `AA:BB:CC:DD:EE:FF`; node B's is `11:22:33:44:55:66`.

**Node A config:**

```yaml
espnow:
  channel: 0              # inherit from wifi: component
  pmk: "my-pmk-16bytes"   # optional; if set, enables LMK encryption
  peers:
    - mac: "11:22:33:44:55:66"
      id: node_b
      lmk: "lmk-for-nodeb"  # optional; omit for unencrypted
  on_receive:
    address: node_b       # filter: only process frames from this peer
    then:
      - if:
          condition:
            lambda: "return data[0] != 0;"
          then:
            - switch.turn_on: relay
          else:
            - switch.turn_off: relay

switch:
  - platform: gpio
    pin: GPIO12
    id: relay
    on_turn_on:
      - espnow.send:
          peer: node_b
          payload: [0x01]
    on_turn_off:
      - espnow.send:
          peer: node_b
          payload: [0x00]
```

Node B's config is symmetric: swap the MAC addresses and the peer id. Because this is a dedicated peer channel (the only traffic between A and B is state updates), no payload-level type discrimination is needed — the address filter is sufficient.

#### Case 2 — Telemetry, explicit primitive composition

**Sensor node:**

```yaml
espnow:
  channel: 6          # explicit channel, no wifi: component on this node
  peers:
    - mac: "HUB:MA:CA:DD:RE:SS"
      id: hub

sensor:
  - platform: dht
    pin: GPIO4
    temperature:
      id: temp
      on_value:
        - espnow.send:
            peer: hub
            payload: !lambda |
              uint8_t buf[4];
              float v = x;
              memcpy(buf, &v, 4);
              return std::vector<uint8_t>(buf, buf + 4);
```

**Hub node:**

```yaml
espnow:
  channel: 6
  peers:
    - mac: "SENSOR:NODE:MAC:AD:DR:ES"
      id: sensor_node
  on_receive:
    address: sensor_node  # filter: only process frames from this sensor
    then:
      - lambda: |
          float v;
          memcpy(&v, data, 4);
          id(remote_temp).publish_state(v);

sensor:
  - platform: template
    id: remote_temp
    name: "Remote Temperature"
    unit_of_measurement: "°C"
    accuracy_decimals: 1
    device_class: temperature
```

The float encoding in these examples is illustrative. If the hub receives from multiple sensors, each with its own peer entry, an `on_receive` block per `address:` handles the routing — no additional type field in the payload is required when each source is dedicated to one data stream.

### 6.2 Declarative Layer

#### Case 1 — Shared state sync, declarative abstraction

```yaml
espnow:
  channel: 0
  peers:
    - mac: "11:22:33:44:55:66"
      id: node_b
      lmk: "lmk-for-nodeb"

switch:
  - platform: gpio
    pin: GPIO12
    id: relay
    espnow_sync:
      peers:
        - node_b
```

The `espnow_sync` key instructs the component to:
1. On `turn_on` or `turn_off`: send the new state to all listed peers.
2. On `on_receive` from any listed peer: apply the received state locally without re-transmitting.

This expands to exactly the flexible-layer configuration shown in section 6.1, case 1.

#### Case 2 — Telemetry, declarative abstraction

**Sensor node:**

```yaml
espnow:
  channel: 6
  peers:
    - mac: "HUB:MA:CA:DD:RE:SS"
      id: hub

sensor:
  - platform: dht
    pin: GPIO4
    temperature:
      id: temp
      espnow_publish:
        peer: hub
```

**Hub node:**

```yaml
espnow:
  channel: 6
  peers:
    - mac: "SENSOR:NODE:MAC:AD:DR:ES"
      id: sensor_node

sensor:
  - platform: espnow
    id: remote_temp
    source: sensor_node     # required: identifies which peer sends this value
    name: "Remote Temperature"
    unit_of_measurement: "°C"
    accuracy_decimals: 1
    device_class: temperature
```

The `platform: espnow` sensor receives frames from the declared `source` peer, decodes the payload using the same encoding the `espnow_publish` mixin uses, and calls `publish_state()`. The `source:` field is required (not optional) for this abstraction — without it the sensor would have no way to distinguish among multiple transmitting nodes. Because both sides use the same declarative abstraction, the encoding is handled internally and consistently.

### 6.3 Demonstrating Primitive Completeness: Heartbeat and Re-sync

These examples show that features intentionally left out of v1 are fully implementable today using the primitives plus ESPHome's existing automation system. No firmware modification is required.

#### Heartbeat / liveness detection

```yaml
# Sender: every 30 s, send a heartbeat (empty payload; this channel is dedicated
# to liveness — no payload discrimination needed)
interval:
  - interval: 30s
    then:
      - espnow.send:
          peer: hub
          payload: []

# Receiver: track peer liveness with a template binary sensor + restart script
binary_sensor:
  - platform: template
    id: peer_alive
    device_class: connectivity

script:
  - id: peer_timeout
    mode: restart         # each new heartbeat restarts the 90 s window
    then:
      - delay: 90s        # 3 missed heartbeats at 30 s interval
      - binary_sensor.template.publish:
          id: peer_alive
          state: false

espnow:
  on_receive:
    address: sensor_node  # filter by source so other peers don't reset the timer
    then:
      - binary_sensor.template.publish:  # standard ESPHome action for template binary sensors
          id: peer_alive
          state: true
      - script.execute: peer_timeout

on_boot:
  then:
    - script.execute: peer_timeout   # start the timeout from boot
```

#### State re-sync on boot

A node that reboots has lost its last-known state. It can request a re-broadcast from peers using a dedicated "state request" topic.

Without topic-based routing, the re-sync request and state update must be distinguished by payload content. By convention, this example reserves the first payload byte as a frame type: `0xFF` = state request, any other value = state update.

```yaml
# On boot, broadcast a state request (first byte 0xFF = "please re-send your state")
on_boot:
  priority: -100.0    # after all components are initialized
  then:
    - espnow.broadcast:
        payload: [0xFF]

# All nodes: respond to state requests and apply incoming state updates
espnow:
  on_broadcast:
    then:
      - if:
          condition:
            lambda: "return data[0] == 0xFF;"
          then:
            - espnow.broadcast:
                payload: !lambda "return {id(relay).state ? (uint8_t)1 : (uint8_t)0};"
  on_receive:
    then:
      - if:
          condition:
            lambda: "return data[0] != 0xFF;"  # ignore re-sync requests on unicast
          then:
            - if:
                condition:
                  lambda: "return data[0] != 0;"
                then:
                  - switch.turn_on: relay
                else:
                  - switch.turn_off: relay
```

The payload-byte convention (`0xFF` as a reserved type marker) is a user-land pattern, not a protocol feature. When topic-based routing is introduced in v2, it will replace this pattern at the declarative layer while keeping the same primitives underneath.

Both examples use the flexible layer and require lambda conditions. The point is not to avoid lambdas in these advanced use cases, but to confirm that the primitive set is sufficient to express them. A user who needs heartbeat or re-sync can implement it today, entirely in YAML, without waiting for a native abstraction.

---

## 7. ESP8266-Specific Considerations

### 7.1 Channel and WiFi Coexistence

The most common deployment has both `wifi:` and `espnow:` active simultaneously. In this configuration:

- The `wifi:` component connects to the AP during `setup()`, before the main loop.
- `esp_now_init()` must be called after `esp_wifi_start()`, which happens inside the WiFi component's setup.
- With `channel: 0` on all peers, ESP-NOW automatically uses the STA channel. No explicit channel management is needed.

The problem case is channel change after reconnection. If the device loses WiFi and reconnects to an AP on a different channel (possible when using mesh systems or multiple APs), existing peers configured with channel 0 adapt automatically because channel 0 is resolved at send time, not at add-peer time. Peers configured with an explicit channel will silently fail to communicate until the mismatch is corrected. This component defaults to channel 0 when `wifi:` is present and makes the explicit-channel option a deliberate override.

**Behavior on AP channel change matches the official component.** When the WiFi connection drops and reconnects on a different channel, recovery is implicit: the `wifi:` component handles reassociation, and once the STA is associated to the new channel, ESP-NOW frames sent with channel 0 will use the correct channel automatically. No active channel tracking is needed in firmware. This is consistent with how the official ESP-NOW component for ESP32 handles the same scenario — both rely on the WiFi stack's reassociation rather than implementing their own channel-change logic.

A secondary concern: in a house with multiple access points, different ESP8266 nodes may connect to different APs on different channels, breaking ESP-NOW communication between them even though they share the same SSID. This is a deployment constraint, not a firmware constraint, but it should be called out in user-facing documentation.

### 7.2 RAM Constraints

The ESP8266 has approximately 80 KB of usable DRAM after SDK overhead, and the heap is fragmented by the WiFi stack. The component must be designed to minimize heap allocations.

Specific implications:

- The receive queue is a fixed-size static ring buffer, not a dynamic container. Queue depth is configurable at compile time via `espnow_queue_depth:` in the YAML, with a sensible default (e.g., 8 frames).
- Each queued frame stores up to 250 bytes of payload plus 6 bytes of source MAC. A queue depth of 8 costs 2 KB of static RAM. This is acceptable.
- The peer list is stored in a static array (maximum 20 entries). No heap allocation for peer management.
- The component should not use `std::string` for MAC addresses in the hot path.

### 7.3 OTA Updates

ESPHome's OTA mechanism uses an HTTP server that the device serves over WiFi. During an OTA update, the device is occupied downloading and writing the new firmware. ESP-NOW frames will continue to arrive in the receive callback and will be lost if the main loop is not running.

The correct behavior is to let frames drop silently during OTA — the queue will overflow and discard frames. After the OTA reboot, the re-sync mechanism (if implemented by the user) can restore state. This is not a failure mode that the component needs to handle internally.

### 7.4 Callback Context

Both the send and receive callbacks execute in the WiFi task, which has higher priority than the Arduino `loop()` task. The receive callback must not:

- Allocate heap memory
- Call ESPHome API functions (which are not thread-safe)
- Block

The implementation uses a statically allocated ring buffer with an atomic write index. The callback writes to the buffer; `loop()` reads from it and dispatches to triggers. This is the standard deferred-work pattern for ESP8266 WiFi callbacks.

### 7.5 Debug and Logging

ESP8266 has no JTAG. All debug output goes through UART. On Sonoff TX devices, UART0 (TX: GPIO1, RX: GPIO3) is typically the programming port and can be used for debug logging at runtime. UART1 (TX: GPIO2) is TX-only and available as an alternative log output.

The component should emit log messages at appropriate levels:
- `DEBUG`: successful sends, received frame metadata (source MAC, length)
- `WARN`: send failures, queue overflow
- `ERROR`: initialization failures

No sensitive payload content should appear in log output at any level.

### 7.6 First-Pairing Workflow

The MAC address of each node must be known before the peer list can be configured. The expected workflow for a new deployment:

1. Flash ESPHome to the first device with any valid configuration (the `espnow:` block can be empty or absent).
2. On first boot, ESPHome logs the device's MAC address at INFO level: `[I][wifi:...]` or similar. Note it.
3. Repeat for all nodes.
4. Populate each node's `espnow: peers:` list with the MACs of the other nodes. Rebuild and reflash all nodes.

This is a one-time setup cost. The MAC address of an ESP8266 is fixed and does not change between flashes unless the user explicitly overrides it. A helper script or ESPHome dashboard feature to automate MAC discovery is out of scope for this component but would be a natural tooling addition.

---

## 8. Open Questions

Resolved questions are retained for reference. Unresolved questions should be closed before the first non-draft version of this document.

**Q1 (resolved): Topic-based routing deferred to v2.**  
The official ESPHome component has no topic concept — payload is raw bytes and routing is left to the user via address filtering and payload inspection. Introducing a topic field in v1 would extend the API beyond the official component without being required by any v1 use case, violating the minimum scope principle. A topic abstraction may be introduced in v2 once concrete use cases demonstrate the need. See Section 9.

**Q2 (resolved): Payload encoding in the flexible layer.**  
Payload accepts a byte array literal (`[0x01, 0x02]`), a plain string (converted to UTF-8 bytes), or a templatable value (lambda returning `std::vector<uint8_t>`). Aligns with the official component.[^4][^5]

**Q3 (resolved): Variables exposed in trigger context.**  
All three receive triggers (`on_receive`, `on_broadcast`, `on_unknown_peer`) expose `info` (struct with `src_addr`), `data` (`const uint8_t*`), and `size` (`uint8_t`), matching the official component's names.[^5][^6] On ESP8266, `info` contains only `src_addr` — RSSI and destination address are unavailable (platform constraint, see Section 2.5). This divergence is documented per the alignment principle.

**Q4 (resolved): Broadcast send and peer registration.**  
`espnow.broadcast` does not require the broadcast address to be in the peer list. If the underlying `esp_now_send()` requires it on ESP8266, the component registers `FF:FF:FF:FF:FF:FF` transparently. Aligns with the official component's behavior.[^4]

**Q5: WiFi channel change at runtime.**  
If the device loses WiFi and reconnects on a different channel, does `channel: 0` in `esp_now_peer_info_t` resolve correctly, or does the stored channel need updating via `esp_now_mod_peer()`? The behavior depends on whether ESP8266 resolves channel 0 at `add_peer` time or at `send` time. Needs empirical verification.

**Q6 (resolved): Multiple handlers for the same trigger.**  
Multiple `on_receive:` (or `on_broadcast:`) blocks are all allowed and all fire, regardless of whether they share the same `address:` filter or have no filter. Standard ESPHome trigger behavior, confirmed by the official component.[^4]

**Q7 (resolved): Sending to an undeclared peer.**  
`espnow.send` to a MAC not in the peer list returns an error. The `auto_add_peer` option (see Primitive 1) governs receive-side auto-registration only; send-side enforcement is strict by default. Aligns with the official component.[^4][^5]

**Q8: SoftAP mode.**  
If the device runs as a SoftAP, `channel: 0` should resolve to the SoftAP channel transparently. Behavior has not been verified on ESP8266.

---

## 9. Out of Scope for v1

Items marked *deferred* are expected to appear in a future version. Items marked *out of scope* are unlikely to be implemented in this component at any version.

**Topic-based routing (deferred to v2).**  
A `topic:` field on `espnow.send`, `espnow.broadcast`, and the receive triggers, enabling message-type multiplexing on a single peer channel without payload-byte conventions. No v1 use case requires it — address-based filtering covers cases 1 and 2 cleanly. Deferred by the minimum scope principle. When added, it will be a purely additive change: v1 configurations remain valid, and the declarative layer will use topic routing internally without exposing it in YAML. The key distinction from the payload-byte convention used in Section 6.3's re-sync example is dispatch level: a topic field is matched by the component before any user handler runs, while a payload byte must be checked in a lambda inside every handler. Topic routing is therefore ergonomically superior when multiple message types flow between the same peers.

**`espnow.set_channel` (deferred to v2).**  
Explicit runtime channel switching, equivalent to the official component's `espnow.set_channel` action. No v1 use case requires manual channel changes — the default `channel: 0` behavior handles the common case. Deferred by minimum scope. Alignment with the official component is desirable but not sufficient justification for inclusion when no concrete problem is solved.

**`enable_on_boot` / `espnow.enable` / `espnow.disable` (deferred).**  
On-demand initialization and teardown of ESP-NOW, equivalent to the official component's `enable_on_boot:` flag and `enable()`/`disable()` methods. Relevant for power-managed deployments that want to bring up ESP-NOW only when needed. No v1 use case requires this. Deferred by minimum scope.

**Remote command abstraction — case 3 (deferred).**  
A declarative `espnow_command:` abstraction for unidirectional remote actuation. The flexible layer already covers this use case: `espnow.send` + `on_receive` + action. A declarative wrapper adds marginal value and duplicates work that ESPHome's automation system already does. Deferred until there is concrete demand.

**Broadcast / pub-sub abstraction — case 4 (deferred).**  
A higher-level pub-sub abstraction for nodes that publish without explicit peer lists. The `espnow.broadcast` action already covers the send side; the receive side is `on_broadcast`. A full pub-sub layer would need to define subscription semantics, which is non-trivial. Deferred.

**Mesh repeater — case 5 (out of scope).**  
Frame forwarding to extend range requires routing decisions, forwarding tables, and loop prevention — a fundamentally different level of complexity from a flat peer model. This would more appropriately be a separate component. Not expected to be implemented here.

**Heartbeat and liveness detection (out of scope).**  
Section 6.3 demonstrates this is fully implementable using v1 primitives plus `interval:` and `script:`. Native support would add firmware complexity for no functional gain. Out of scope by the minimum logic in firmware principle.

**Re-sync on boot (out of scope).**  
Same argument: expressible via `on_boot:` and `espnow.broadcast` with payload-byte conventions, as shown in Section 6.3. Out of scope.

**Conflict resolution beyond last-write-wins (out of scope).**  
For the target use case — physical switch presses in a residential environment — simultaneous conflicting writes are not a realistic concern. CRDT-style or timestamp-based resolution would add firmware complexity with no practical benefit for the intended deployment context.

**ESP32 support (out of scope).**  
ESPHome's official ESP-NOW component covers ESP32. This component targets the gap: ESP8266 and ESP8285 devices that the official component cannot serve.

**Non-Arduino framework (out of scope by constraint).**  
ESPHome only supports the Arduino framework for ESP8266. There is no RTOS SDK / IDF path available.

---

## 10. References

[^1]: Espressif. *ESP-NOW — ESP-IDF Programming Guide (ESP32)*. https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html  
[^2]: Espressif. *ESP8266 RTOS SDK — `esp_now.h`*. https://github.com/espressif/ESP8266_RTOS_SDK/blob/master/components/esp8266/include/esp_now.h  
[^3]: Espressif. *ESP-IDF — `esp_now.h`*. https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_now.h  
[^4]: ESPHome. *ESP-NOW Component — User Documentation*. https://esphome.io/components/espnow/  
[^5]: ESPHome. *ESP-NOW Component — `__init__.py`*. https://github.com/esphome/esphome/blob/dev/esphome/components/espnow/__init__.py  
[^6]: ESPHome. *ESP-NOW Component — `automation.h`*. https://github.com/esphome/esphome/blob/dev/esphome/components/espnow/automation.h  

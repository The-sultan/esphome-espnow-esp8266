#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#ifdef USE_ESP8266

#include <array>
#include <cstring>
#include <functional>
#include <vector>

extern "C" {
#include "esp_now.h"
#include "esp_wifi.h"
}

namespace esphome {
namespace espnow {

static const uint8_t ESPNOW_MAX_DATA_LEN = 250;
static const uint8_t ESPNOW_MAX_TOTAL_PEER_NUM = 20;
static const uint8_t ESPNOW_QUEUE_SIZE = 8;

using peer_address_t = std::array<uint8_t, 6>;
using send_callback_t = std::function<void(esp_err_t)>;

/// Received frame metadata.
/// On ESP8266, only src_addr is available — the receive callback does not expose
/// des_addr (ESP8266 RTOS SDK limitation, see design doc §2.5 and §4 divergences).
struct ESPNowRecvInfo {
  uint8_t src_addr[6];
};

struct ESPNowPeerConfig {
  uint8_t mac[6];
  uint8_t channel;
  bool encrypt;
  uint8_t lmk[16];
};

struct ESPNowQueueEntry {
  ESPNowRecvInfo info;
  uint8_t data[ESPNOW_MAX_DATA_LEN];
  uint8_t size;
};

// ── Handler interfaces (matching official ESP-NOW component naming) ────────────

class ESPNowReceivePacketHandler {
 public:
  virtual bool on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) = 0;
};

class ESPNowBroadcastHandler {
 public:
  virtual bool on_broadcast(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) = 0;
};

class ESPNowUnknownPeerHandler {
 public:
  virtual bool on_unknown_peer(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) = 0;
};

// ── Component ─────────────────────────────────────────────────────────────────

class ESPNowComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_channel(uint8_t channel) { channel_ = channel; }
  void set_pmk(const std::vector<uint8_t> &pmk);
  void set_auto_add_peer(bool v) { auto_add_peer_ = v; }

  void add_static_peer(peer_address_t mac, uint8_t channel);
  void add_static_peer_with_lmk(peer_address_t mac, uint8_t channel, std::array<uint8_t, 16> lmk);

  esp_err_t add_peer(const uint8_t *mac);
  esp_err_t del_peer(const uint8_t *mac);

  esp_err_t send(const uint8_t *peer, const std::vector<uint8_t> &data,
                 const send_callback_t &callback = nullptr) {
    return this->send(peer, data.data(), data.size(), callback);
  }
  esp_err_t send(const uint8_t *peer, const uint8_t *data, size_t size,
                 const send_callback_t &callback = nullptr);

  void register_receive_handler(ESPNowReceivePacketHandler *h) { receive_handlers_.push_back(h); }
  void register_broadcast_handler(ESPNowBroadcastHandler *h) { broadcast_handlers_.push_back(h); }
  void register_unknown_peer_handler(ESPNowUnknownPeerHandler *h) { unknown_peer_handlers_.push_back(h); }

 protected:
  static void recv_cb_(const uint8_t *mac, const uint8_t *data, int len);
  static void send_cb_(const uint8_t *mac, esp_now_send_status_t status);
  void dispatch_(const ESPNowQueueEntry &entry);

  uint8_t channel_{0};
  bool has_pmk_{false};
  uint8_t pmk_[16]{};
  bool auto_add_peer_{false};

  ESPNowPeerConfig static_peers_[ESPNOW_MAX_TOTAL_PEER_NUM];
  uint8_t static_peer_count_{0};

  // Single-producer (WiFi task) / single-consumer (loop) ring buffer.
  // write_ advanced only by recv_cb_, read_ only by loop() — lock-free on single core.
  static ESPNowQueueEntry recv_queue_[ESPNOW_QUEUE_SIZE];
  static volatile uint8_t recv_queue_write_;
  uint8_t recv_queue_read_{0};

  // Send callback deferred to loop() — WiFi task sets the flag, loop() invokes the fn.
  static volatile bool send_cb_fired_;
  static volatile esp_now_send_status_t send_cb_status_;
  send_callback_t pending_send_cb_{nullptr};

  std::vector<ESPNowReceivePacketHandler *> receive_handlers_;
  std::vector<ESPNowBroadcastHandler *> broadcast_handlers_;
  std::vector<ESPNowUnknownPeerHandler *> unknown_peer_handlers_;
};

extern ESPNowComponent *global_esp_now;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ── Triggers ──────────────────────────────────────────────────────────────────

class OnReceiveTrigger : public Trigger<const ESPNowRecvInfo &, const uint8_t *, uint8_t>,
                         public ESPNowReceivePacketHandler {
 public:
  explicit OnReceiveTrigger(peer_address_t address) : has_address_(true) {
    memcpy(address_, address.data(), 6);
  }
  explicit OnReceiveTrigger() {}

  bool on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override {
    if (has_address_ && memcmp(address_, info.src_addr, 6) != 0)
      return false;
    this->trigger(info, data, size);
    return false;
  }

 protected:
  bool has_address_{false};
  uint8_t address_[6]{};
};

class OnBroadcastTrigger : public Trigger<const ESPNowRecvInfo &, const uint8_t *, uint8_t>,
                           public ESPNowBroadcastHandler {
 public:
  explicit OnBroadcastTrigger(peer_address_t address) : has_address_(true) {
    memcpy(address_, address.data(), 6);
  }
  explicit OnBroadcastTrigger() {}

  bool on_broadcast(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override {
    if (has_address_ && memcmp(address_, info.src_addr, 6) != 0)
      return false;
    this->trigger(info, data, size);
    return false;
  }

 protected:
  bool has_address_{false};
  uint8_t address_[6]{};
};

class OnUnknownPeerTrigger : public Trigger<const ESPNowRecvInfo &, const uint8_t *, uint8_t>,
                             public ESPNowUnknownPeerHandler {
 public:
  bool on_unknown_peer(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override {
    this->trigger(info, data, size);
    return false;
  }
};

// ── Actions ───────────────────────────────────────────────────────────────────

template<typename... Ts>
class SendAction : public Action<Ts...>, public Parented<ESPNowComponent> {
  TEMPLATABLE_VALUE(peer_address_t, address)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)

 public:
  void set_wait_for_sent(bool v) { wait_for_sent_ = v; }
  void set_continue_on_error(bool v) { continue_on_error_ = v; }

  void play_complex(const Ts &...x) override {
    this->num_running_++;
    auto address = this->address_.value(x...);
    auto data = this->data_.value(x...);
    bool wait = wait_for_sent_;
    bool cont = continue_on_error_;

    send_callback_t cb = [this, wait, cont, x...](esp_err_t status) {
      if (status == ESP_OK) {
        if (wait)
          this->play_next_(x...);
      } else {
        if (cont) {
          if (wait)
            this->play_next_(x...);
        } else {
          this->stop_complex();
        }
      }
    };

    esp_err_t err = this->parent_->send(address.data(), data, cb);
    if (err != ESP_OK) {
      cb(err);
    } else if (!wait) {
      this->play_next_(x...);
    }
  }

 protected:
  void play(const Ts &...x) override {}

  bool wait_for_sent_{true};
  bool continue_on_error_{true};
};

template<typename... Ts>
class AddPeerAction : public Action<Ts...>, public Parented<ESPNowComponent> {
  TEMPLATABLE_VALUE(peer_address_t, address)

 protected:
  void play(const Ts &...x) override {
    auto addr = this->address_.value(x...);
    this->parent_->add_peer(addr.data());
  }
};

template<typename... Ts>
class DeletePeerAction : public Action<Ts...>, public Parented<ESPNowComponent> {
  TEMPLATABLE_VALUE(peer_address_t, address)

 protected:
  void play(const Ts &...x) override {
    auto addr = this->address_.value(x...);
    this->parent_->del_peer(addr.data());
  }
};

}  // namespace espnow
}  // namespace esphome

// ── Declarative layer: switch sync ───────────────────────────────────────────
// Included only when the switch component is compiled into the build.

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace espnow {

/// Binds an ESPHome switch to a set of ESP-NOW peers for symmetric state sync.
///
/// On construction, registers an on_state_callback on the switch that sends the
/// new state (0x01 = on, 0x00 = off) to every peer.  As an ESPNowReceivePacketHandler,
/// it receives incoming state frames from those same peers and applies them locally
/// without re-transmitting — avoiding feedback loops.
///
/// Expands the declarative espnow: sync_switches: abstraction from the design doc §6.2.
class SwitchSyncGroup : public ESPNowReceivePacketHandler {
 public:
  SwitchSyncGroup(ESPNowComponent *espnow, switch_::Switch *sw) : espnow_(espnow), sw_(sw) {
    sw->add_on_state_callback([this](bool state) {
      uint8_t payload = state ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
      for (uint8_t i = 0; i < peer_count_; i++) {
        espnow_->send(peers_[i], &payload, 1, nullptr);
      }
    });
  }

  void add_peer(peer_address_t peer) {
    if (peer_count_ < ESPNOW_MAX_TOTAL_PEER_NUM) {
      memcpy(peers_[peer_count_++], peer.data(), 6);
    }
  }

  bool on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override {
    for (uint8_t i = 0; i < peer_count_; i++) {
      if (memcmp(info.src_addr, peers_[i], 6) == 0) {
        if (size >= 1) {
          if (data[0] != 0) sw_->turn_on();
          else sw_->turn_off();
        }
        return false;
      }
    }
    return false;
  }

 protected:
  ESPNowComponent *espnow_;
  switch_::Switch *sw_;
  uint8_t peers_[ESPNOW_MAX_TOTAL_PEER_NUM][6]{};
  uint8_t peer_count_{0};
};

}  // namespace espnow
}  // namespace esphome
#endif  // USE_SWITCH

// ── Declarative layer: sensor publish / receive ───────────────────────────────
// Included only when the sensor component is compiled into the build.

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace espnow {

/// Receive-side sensor for the declarative espnow: telemetry abstraction.
///
/// Registers itself as a receive handler with the ESP-NOW component.  When a
/// frame arrives from the configured source peer, decodes the 4-byte IEEE 754
/// float payload and calls publish_state().  Only frames from source_ are
/// accepted; frames from other peers are passed through (return false).
///
/// The float encoding is the same one espnow_publish uses on the sender side,
/// making the pairing symmetric without user-visible encoding details.
class ESPNowSensor : public sensor::Sensor, public ESPNowReceivePacketHandler {
 public:
  ESPNowSensor(ESPNowComponent *espnow, peer_address_t source) {
    memcpy(source_, source.data(), 6);
    espnow->register_receive_handler(this);
  }

  bool on_receive(const ESPNowRecvInfo &info, const uint8_t *data, uint8_t size) override {
    if (memcmp(info.src_addr, source_, 6) != 0)
      return false;
    if (size != sizeof(float))
      return false;
    float value;
    memcpy(&value, data, sizeof(float));
    this->publish_state(value);
    return false;
  }

 protected:
  uint8_t source_[6]{};
};

/// Sender-side helper for espnow_publish sensor mixin.
///
/// Wraps the send call so it can be registered as an on_state_callback without
/// requiring a lambda in the generated code.
class SensorPublishHandler {
 public:
  SensorPublishHandler(ESPNowComponent *espnow, peer_address_t peer) : espnow_(espnow) {
    memcpy(peer_, peer.data(), 6);
  }

  /// Register this handler as a state callback on the given sensor.
  /// Avoids lambdas in the generated code — called once from setup().
  void register_with(sensor::Sensor *sensor) {
    sensor->add_on_state_callback([this](float value) {
      uint8_t buf[sizeof(float)];
      memcpy(buf, &value, sizeof(float));
      espnow_->send(peer_, buf, sizeof(float), nullptr);
    });
  }

 protected:
  ESPNowComponent *espnow_;
  uint8_t peer_[6]{};
};

}  // namespace espnow
}  // namespace esphome
#endif  // USE_SENSOR

#endif  // USE_ESP8266

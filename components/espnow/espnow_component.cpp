#include "espnow_component.h"

#ifdef USE_ESP8266

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <cstring>

namespace esphome {
namespace espnow {

static const char *const TAG = "espnow";
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Static member definitions ─────────────────────────────────────────────────

ESPNowComponent *global_esp_now = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ESPNowQueueEntry ESPNowComponent::recv_queue_[ESPNOW_QUEUE_SIZE]{};
volatile uint8_t ESPNowComponent::recv_queue_write_ = 0;
volatile bool ESPNowComponent::send_cb_fired_ = false;
volatile uint8_t ESPNowComponent::send_cb_status_ = 0;

// ── Setup ─────────────────────────────────────────────────────────────────────

void ESPNowComponent::setup() {
  global_esp_now = this;

  int err = esp_now_init();
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_init failed: %d", err);
    this->mark_failed();
    return;
  }

  // On ESP8266, the node must declare a role before sending/receiving.
  // COMBO means it can both send (controller) and receive (slave).
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);

  err = esp_now_register_recv_cb(recv_cb_);
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  err = esp_now_register_send_cb(send_cb_);
  if (err != 0) {
    ESP_LOGE(TAG, "esp_now_register_send_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  if (has_pmk_) {
    // On ESP8266, the network key is called KoK (Key of Keys).
    esp_now_set_kok(pmk_, 16);
  }

  // Register the broadcast address so espnow.broadcast works without
  // requiring the user to list FF:FF:FF:FF:FF:FF in their peer config.
  if (!esp_now_is_peer_exist(const_cast<uint8_t *>(BROADCAST_ADDR))) {
    esp_now_add_peer(const_cast<uint8_t *>(BROADCAST_ADDR), ESP_NOW_ROLE_COMBO, channel_, nullptr, 0);
  }

  for (uint8_t i = 0; i < static_peer_count_; i++) {
    const ESPNowPeerConfig &cfg = static_peers_[i];
    uint8_t *key = cfg.encrypt ? const_cast<uint8_t *>(cfg.lmk) : nullptr;
    uint8_t key_len = cfg.encrypt ? 16 : 0;
    err = esp_now_add_peer(const_cast<uint8_t *>(cfg.mac), ESP_NOW_ROLE_COMBO, cfg.channel, key, key_len);
    if (err != 0) {
      ESP_LOGW(TAG, "Failed to add static peer %02X:%02X:%02X:%02X:%02X:%02X: %d",
               cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5], err);
    }
  }

  ESP_LOGD(TAG, "ESP-NOW initialized, %u static peer(s)", static_peer_count_);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void ESPNowComponent::loop() {
  while (recv_queue_read_ != recv_queue_write_) {
    dispatch_(recv_queue_[recv_queue_read_]);
    recv_queue_read_ = (recv_queue_read_ + 1) % ESPNOW_QUEUE_SIZE;
  }

  if (send_cb_fired_ && pending_send_cb_) {
    send_cb_fired_ = false;
    auto cb = pending_send_cb_;
    pending_send_cb_ = nullptr;
    ESP_LOGD(TAG, "TX ack: %s", send_cb_status_ == 0 ? "OK" : "FAIL");
    cb(send_cb_status_ == 0 ? 0 : -1);  // 0 = ESP_NOW_SEND_SUCCESS on ESP8266
  }
}

void ESPNowComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP-NOW:");
  ESP_LOGCONFIG(TAG, "  Channel: %u (0 = inherit from wifi)", channel_);
  ESP_LOGCONFIG(TAG, "  PMK: %s", has_pmk_ ? "set" : "not set");
  ESP_LOGCONFIG(TAG, "  Auto-add peers: %s", auto_add_peer_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Static peers: %u", static_peer_count_);
  for (uint8_t i = 0; i < static_peer_count_; i++) {
    const uint8_t *m = static_peers_[i].mac;
    ESP_LOGCONFIG(TAG, "    %02X:%02X:%02X:%02X:%02X:%02X%s",
                  m[0], m[1], m[2], m[3], m[4], m[5],
                  static_peers_[i].encrypt ? " (encrypted)" : "");
  }
}

// ── Peer management ───────────────────────────────────────────────────────────

void ESPNowComponent::set_pmk(const std::array<uint8_t, 16> &pmk) {
  has_pmk_ = true;
  memcpy(pmk_, pmk.data(), 16);
}

void ESPNowComponent::add_static_peer(peer_address_t mac, uint8_t channel) {
  if (static_peer_count_ >= ESPNOW_MAX_TOTAL_PEER_NUM) {
    ESP_LOGW(TAG, "Maximum peer count (%u) reached", ESPNOW_MAX_TOTAL_PEER_NUM);
    return;
  }
  ESPNowPeerConfig &cfg = static_peers_[static_peer_count_++];
  memcpy(cfg.mac, mac.data(), 6);
  cfg.channel = channel;
  cfg.encrypt = false;
  memset(cfg.lmk, 0, 16);
}

void ESPNowComponent::add_static_peer_with_lmk(peer_address_t mac, uint8_t channel,
                                                std::array<uint8_t, 16> lmk) {
  if (static_peer_count_ >= ESPNOW_MAX_TOTAL_PEER_NUM) {
    ESP_LOGW(TAG, "Maximum peer count (%u) reached", ESPNOW_MAX_TOTAL_PEER_NUM);
    return;
  }
  ESPNowPeerConfig &cfg = static_peers_[static_peer_count_++];
  memcpy(cfg.mac, mac.data(), 6);
  cfg.channel = channel;
  cfg.encrypt = true;
  memcpy(cfg.lmk, lmk.data(), 16);
}

int ESPNowComponent::add_peer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(const_cast<uint8_t *>(mac)))
    return 0;

  int err = esp_now_add_peer(const_cast<uint8_t *>(mac), ESP_NOW_ROLE_COMBO, channel_, nullptr, 0);
  if (err != 0) {
    ESP_LOGW(TAG, "add_peer %02X:%02X:%02X:%02X:%02X:%02X failed: %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], err);
  }
  return err;
}

int ESPNowComponent::del_peer(const uint8_t *mac) {
  int err = esp_now_del_peer(const_cast<uint8_t *>(mac));
  if (err != 0) {
    ESP_LOGW(TAG, "del_peer %02X:%02X:%02X:%02X:%02X:%02X failed: %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], err);
  }
  return err;
}

// ── Send ──────────────────────────────────────────────────────────────────────

int ESPNowComponent::send(const uint8_t *peer, const uint8_t *data, size_t size,
                          const send_callback_t &callback) {
  if (this->is_failed())
    return -1;

  if (size > ESPNOW_MAX_DATA_LEN) {
    ESP_LOGW(TAG, "Payload too large (%u > %u bytes)", (unsigned) size, ESPNOW_MAX_DATA_LEN);
    return -1;
  }

  if (!esp_now_is_peer_exist(const_cast<uint8_t *>(peer))) {
    if (auto_add_peer_) {
      this->add_peer(peer);
    } else {
      ESP_LOGW(TAG, "send to unregistered peer %02X:%02X:%02X:%02X:%02X:%02X",
               peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
      return -1;
    }
  }

  if (pending_send_cb_ != nullptr) {
    ESP_LOGW(TAG, "send called while previous send is pending — dropping");
    return -1;
  }

  pending_send_cb_ = callback;
  send_cb_fired_ = false;

  // esp_now_send takes non-const pointers on ESP8266
  int err = esp_now_send(const_cast<uint8_t *>(peer), const_cast<uint8_t *>(data), (int) size);
  if (err != 0) {
    ESP_LOGW(TAG, "esp_now_send failed: %d", err);
    pending_send_cb_ = nullptr;
    return err;
  }

  ESP_LOGD(TAG, "TX to %02X:%02X:%02X:%02X:%02X:%02X len=%u",
           peer[0], peer[1], peer[2], peer[3], peer[4], peer[5], (unsigned) size);
  return 0;
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ESPNowComponent::dispatch_(const ESPNowQueueEntry &entry) {
  const uint8_t *src = entry.info.src_addr;

  ESP_LOGD(TAG, "RX from %02X:%02X:%02X:%02X:%02X:%02X len=%u",
           src[0], src[1], src[2], src[3], src[4], src[5], entry.size);

  if (esp_now_is_peer_exist(const_cast<uint8_t *>(src))) {
    for (auto *h : receive_handlers_) {
      if (h->on_receive(entry.info, entry.data, entry.size))
        break;
    }
  } else {
    // On ESP8266, des_addr is not available in the receive callback, so broadcast
    // and unknown-unicast frames are indistinguishable. Frames from unregistered
    // sources fire both on_broadcast and on_unknown_peer.
    for (auto *h : broadcast_handlers_) {
      if (h->on_broadcast(entry.info, entry.data, entry.size))
        break;
    }
    for (auto *h : unknown_peer_handlers_) {
      if (h->on_unknown_peer(entry.info, entry.data, entry.size))
        break;
    }
    if (auto_add_peer_) {
      this->add_peer(src);
    }
  }
}

// ── Static callbacks (WiFi task — must not allocate or call ESPHome APIs) ─────

void ICACHE_RAM_ATTR ESPNowComponent::recv_cb_(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (global_esp_now == nullptr || len == 0 || len > ESPNOW_MAX_DATA_LEN)
    return;

  uint8_t next = (recv_queue_write_ + 1) % ESPNOW_QUEUE_SIZE;
  if (next == global_esp_now->recv_queue_read_)
    return;  // queue full — frame dropped

  ESPNowQueueEntry &entry = recv_queue_[recv_queue_write_];
  memcpy(entry.info.src_addr, mac, 6);
  memcpy(entry.data, data, len);
  entry.size = len;

  recv_queue_write_ = next;
}

void ICACHE_RAM_ATTR ESPNowComponent::send_cb_(uint8_t *mac, uint8_t status) {
  if (global_esp_now == nullptr)
    return;
  send_cb_status_ = status;
  send_cb_fired_ = true;
}

}  // namespace espnow
}  // namespace esphome

#endif  // USE_ESP8266

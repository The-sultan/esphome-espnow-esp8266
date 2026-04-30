#include "espnow_component.h"

#ifdef USE_ESP8266

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <cstring>

extern "C" {
#include "esp_wifi.h"
}

namespace esphome {
namespace espnow {

static const char *const TAG = "espnow";
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Static member definitions ─────────────────────────────────────────────────

ESPNowComponent *global_esp_now = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

ESPNowQueueEntry ESPNowComponent::recv_queue_[ESPNOW_QUEUE_SIZE]{};
volatile uint8_t ESPNowComponent::recv_queue_write_ = 0;
volatile bool ESPNowComponent::send_cb_fired_ = false;
volatile esp_now_send_status_t ESPNowComponent::send_cb_status_ = ESP_NOW_SEND_SUCCESS;

// ── Setup ─────────────────────────────────────────────────────────────────────

void ESPNowComponent::setup() {
  global_esp_now = this;

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_init failed: %d", err);
    this->mark_failed();
    return;
  }

  err = esp_now_register_recv_cb(recv_cb_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  err = esp_now_register_send_cb(send_cb_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_register_send_cb failed: %d", err);
    this->mark_failed();
    return;
  }

  if (has_pmk_) {
    esp_now_set_pmk(pmk_);
  }

  // Register the broadcast address as a peer so espnow.broadcast works without
  // requiring the user to list FF:FF:FF:FF:FF:FF in their peer config.
  if (!esp_now_is_peer_exist(BROADCAST_ADDR)) {
    esp_now_peer_info_t bcast{};
    memcpy(bcast.peer_addr, BROADCAST_ADDR, 6);
    bcast.channel = 0;
    bcast.encrypt = false;
    bcast.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&bcast);
  }

  for (uint8_t i = 0; i < static_peer_count_; i++) {
    const ESPNowPeerConfig &cfg = static_peers_[i];
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, cfg.mac, 6);
    peer.channel = cfg.channel;
    peer.encrypt = cfg.encrypt;
    peer.ifidx = WIFI_IF_STA;
    if (cfg.encrypt) {
      memcpy(peer.lmk, cfg.lmk, 16);
    }
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to add static peer " MACSTR ": %d", MAC2STR(cfg.mac), err);
    }
  }

  ESP_LOGD(TAG, "ESP-NOW initialized, %u static peer(s)", static_peer_count_);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void ESPNowComponent::loop() {
  // Drain the receive queue
  while (recv_queue_read_ != recv_queue_write_) {
    dispatch_(recv_queue_[recv_queue_read_]);
    recv_queue_read_ = (recv_queue_read_ + 1) % ESPNOW_QUEUE_SIZE;
  }

  // Fire deferred send callback — set by send_cb_ in WiFi task, consumed here
  if (send_cb_fired_ && pending_send_cb_) {
    send_cb_fired_ = false;
    auto cb = pending_send_cb_;
    pending_send_cb_ = nullptr;
    cb(send_cb_status_ == ESP_NOW_SEND_SUCCESS ? ESP_OK : ESP_FAIL);
  }
}

void ESPNowComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP-NOW:");
  ESP_LOGCONFIG(TAG, "  Channel: %u (0 = inherit from wifi)", channel_);
  ESP_LOGCONFIG(TAG, "  PMK: %s", has_pmk_ ? "set" : "not set");
  ESP_LOGCONFIG(TAG, "  Auto-add peers: %s", auto_add_peer_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Static peers: %u", static_peer_count_);
  for (uint8_t i = 0; i < static_peer_count_; i++) {
    ESP_LOGCONFIG(TAG, "    " MACSTR "%s", MAC2STR(static_peers_[i].mac),
                  static_peers_[i].encrypt ? " (encrypted)" : "");
  }
}

// ── Peer management ───────────────────────────────────────────────────────────

void ESPNowComponent::set_pmk(const std::vector<uint8_t> &pmk) {
  if (pmk.size() != 16) {
    ESP_LOGE(TAG, "PMK must be exactly 16 bytes, got %u", (unsigned) pmk.size());
    return;
  }
  has_pmk_ = true;
  memcpy(pmk_, pmk.data(), 16);
}

void ESPNowComponent::add_static_peer(peer_address_t mac, uint8_t channel) {
  if (static_peer_count_ >= ESPNOW_MAX_TOTAL_PEER_NUM) {
    ESP_LOGW(TAG, "Maximum peer count (%u) reached, cannot add " MACSTR,
             ESPNOW_MAX_TOTAL_PEER_NUM, MAC2STR(mac.data()));
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
    ESP_LOGW(TAG, "Maximum peer count (%u) reached, cannot add " MACSTR,
             ESPNOW_MAX_TOTAL_PEER_NUM, MAC2STR(mac.data()));
    return;
  }
  ESPNowPeerConfig &cfg = static_peers_[static_peer_count_++];
  memcpy(cfg.mac, mac.data(), 6);
  cfg.channel = channel;
  cfg.encrypt = true;
  memcpy(cfg.lmk, lmk.data(), 16);
}

esp_err_t ESPNowComponent::add_peer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac))
    return ESP_OK;

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel_;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;

  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "add_peer " MACSTR " failed: %d", MAC2STR(mac), err);
  }
  return err;
}

esp_err_t ESPNowComponent::del_peer(const uint8_t *mac) {
  esp_err_t err = esp_now_del_peer(mac);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "del_peer " MACSTR " failed: %d", MAC2STR(mac), err);
  }
  return err;
}

// ── Send ──────────────────────────────────────────────────────────────────────

esp_err_t ESPNowComponent::send(const uint8_t *peer, const uint8_t *data, size_t size,
                                const send_callback_t &callback) {
  if (this->is_failed())
    return ESP_FAIL;

  if (size > ESPNOW_MAX_DATA_LEN) {
    ESP_LOGW(TAG, "Payload too large (%u > %u bytes)", (unsigned) size, ESPNOW_MAX_DATA_LEN);
    return ESP_ERR_INVALID_SIZE;
  }

  // Auto-register the broadcast address if needed
  if (memcmp(peer, BROADCAST_ADDR, 6) == 0 && !esp_now_is_peer_exist(BROADCAST_ADDR)) {
    this->add_peer(BROADCAST_ADDR);
  }

  if (!esp_now_is_peer_exist(peer) && !auto_add_peer_) {
    ESP_LOGW(TAG, "send to unregistered peer " MACSTR, MAC2STR(peer));
    return ESP_ERR_ESPNOW_NOT_FOUND;
  }
  if (!esp_now_is_peer_exist(peer) && auto_add_peer_) {
    this->add_peer(peer);
  }

  if (pending_send_cb_ != nullptr) {
    ESP_LOGW(TAG, "send called while previous send is pending — dropping");
    return ESP_ERR_ESPNOW_NO_MEM;
  }

  pending_send_cb_ = callback;
  send_cb_fired_ = false;

  esp_err_t err = esp_now_send(peer, data, size);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_now_send failed: %d", err);
    pending_send_cb_ = nullptr;
    return err;
  }

  ESP_LOGD(TAG, "TX to " MACSTR " len=%u", MAC2STR(peer), (unsigned) size);
  return ESP_OK;
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ESPNowComponent::dispatch_(const ESPNowQueueEntry &entry) {
  const uint8_t *src = entry.info.src_addr;

  ESP_LOGD(TAG, "RX from " MACSTR " len=%u", MAC2STR(src), entry.size);

  if (esp_now_is_peer_exist(src)) {
    // Unicast from a registered peer → receive handlers.
    for (auto *h : receive_handlers_) {
      if (h->on_receive(entry.info, entry.data, entry.size))
        break;
    }
  } else {
    // Unknown source.
    // On ESP8266, des_addr is not available in the receive callback, so broadcast
    // and unknown-unicast frames are indistinguishable. Frames from unregistered
    // sources fire both on_broadcast and on_unknown_peer (see design doc §4 divergences).
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

void ICACHE_RAM_ATTR ESPNowComponent::recv_cb_(const uint8_t *mac, const uint8_t *data, int len) {
  if (global_esp_now == nullptr || len <= 0 || len > ESPNOW_MAX_DATA_LEN)
    return;

  uint8_t next = (recv_queue_write_ + 1) % ESPNOW_QUEUE_SIZE;
  if (next == global_esp_now->recv_queue_read_) {
    return;  // queue full — frame dropped; overflow logged by loop() if needed
  }

  ESPNowQueueEntry &entry = recv_queue_[recv_queue_write_];
  memcpy(entry.info.src_addr, mac, 6);
  memcpy(entry.data, data, len);
  entry.size = static_cast<uint8_t>(len);

  recv_queue_write_ = next;
}

void ICACHE_RAM_ATTR ESPNowComponent::send_cb_(const uint8_t *mac, esp_now_send_status_t status) {
  if (global_esp_now == nullptr)
    return;
  send_cb_status_ = status;
  send_cb_fired_ = true;
}

}  // namespace espnow
}  // namespace esphome

#endif  // USE_ESP8266

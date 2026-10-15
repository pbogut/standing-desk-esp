#pragma once

#include <stdint.h>
#include <string.h>

struct WifiCredentials {
  char ssid[33] = {};
  char password[64] = {};
  static constexpr unsigned recordSize = 105;

  bool valid() const {
    const char *ssidEnd = static_cast<const char *>(memchr(ssid, 0, sizeof(ssid)));
    const char *passEnd = static_cast<const char *>(memchr(password, 0, sizeof(password)));
    return ssidEnd && ssidEnd != ssid && passEnd
        && (passEnd == password || passEnd - password >= 8);
  }

  bool set(const char *name, const char *pass) {
    const unsigned nameLength = strlen(name), passLength = strlen(pass);
    if (!nameLength || nameLength > 32 || passLength > 63
        || (passLength && passLength < 8)) return false;
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    memcpy(ssid, name, nameLength);
    memcpy(password, pass, passLength);
    return true;
  }

  void encode(uint8_t (&record)[recordSize]) const {
    memcpy(record, "DWF1", 4);
    memcpy(record + 4, ssid, sizeof(ssid));
    memcpy(record + 37, password, sizeof(password));
    const uint32_t sum = checksum(record);
    for (unsigned i = 0; i < 4; ++i) record[101 + i] = sum >> (i * 8);
  }

  bool decode(const uint8_t (&record)[recordSize]) {
    *this = WifiCredentials();
    uint32_t stored = 0;
    for (unsigned i = 0; i < 4; ++i) stored |= uint32_t(record[101 + i]) << (i * 8);
    if (memcmp(record, "DWF1", 4) || checksum(record) != stored) return false;
    WifiCredentials loaded;
    memcpy(loaded.ssid, record + 4, sizeof(ssid));
    memcpy(loaded.password, record + 37, sizeof(password));
    if (!loaded.valid()) return false;
    *this = loaded;
    return true;
  }

 private:
  static uint32_t checksum(const uint8_t *record) {
    uint32_t hash = 2166136261UL;
    for (unsigned i = 0; i < 101; ++i) hash = (hash ^ record[i]) * 16777619UL;
    return hash;
  }
};

class WifiConnection {
 public:
  static constexpr uint32_t fallbackMs = 60000, retryMs = 30000;
  static constexpr uint32_t apGraceMs = 30000, apRetryMs = 10000;
  bool configured = false, connected = false, apActive = false;

  const char *stateName() const {
    if (connected) return "connected";
    if (apActive) return configured ? "fallback_ap" : "setup_ap";
    return configured ? "connecting" : "ap_failed";
  }

  uint32_t fallbackRemaining(uint32_t now) const {
    if (!configured || connected || apActive) return 0;
    const uint32_t elapsed = now - offlineMs_;
    return elapsed < fallbackMs ? fallbackMs - elapsed : 0;
  }

  void extendApGrace(uint32_t now) {
    if (connected && apActive) connectedMs_ = now;
  }

  template <typename Network>
  void configure(bool saved, Network &network, uint32_t now) {
    configured = saved;
    connected = false;
    connectionsAllowed_ = true;
    reconnectPending_ = false;
    offlineMs_ = attemptMs_ = now;
    if (saved) network.connect(apActive);
    else {
      network.disconnect();
      ensureAp(network, now, true);
    }
  }

  template <typename Network>
  void update(Network &network, uint32_t now, bool allowConnections = true) {
    const bool resuming = !connectionsAllowed_ && allowConnections;
    connectionsAllowed_ = allowConnections;
    if (!configured) { ensureAp(network, now, false); return; }
    const bool online = network.isConnected();
    if (online) {
      if (!connected) connectedMs_ = now;
      connected = true;
      if (allowConnections && apActive && uint32_t(now - connectedMs_) >= apGraceMs) {
        network.stopAp();
        apActive = false;
      }
      return;
    }
    if (connected) {
      connected = false;
      offlineMs_ = attemptMs_ = now;
      reconnectPending_ = true;
    }
    if (uint32_t(now - offlineMs_) >= fallbackMs) ensureAp(network, now, false);
    if (allowConnections && (reconnectPending_ || resuming
        || uint32_t(now - attemptMs_) >= retryMs)) {
      attemptMs_ = now;
      reconnectPending_ = false;
      network.connect(apActive);
    }
  }

 private:
  uint32_t offlineMs_ = 0, attemptMs_ = 0, connectedMs_ = 0, apAttemptMs_ = 0;
  bool apAttempted_ = false;
  bool connectionsAllowed_ = true, reconnectPending_ = false;

  template <typename Network>
  void ensureAp(Network &network, uint32_t now, bool immediate) {
    if (apActive) return;
    if (!immediate && apAttempted_ && uint32_t(now - apAttemptMs_) < apRetryMs) return;
    apAttempted_ = true;
    apAttemptMs_ = now;
    apActive = network.startAp(configured);
  }
};

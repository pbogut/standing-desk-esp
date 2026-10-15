#pragma once

#include <stdint.h>
#include <string.h>

class WifiScan {
 public:
  static constexpr unsigned maxNetworks = 20;
  static constexpr uint32_t timeoutMs = 15000;
  enum class State { Idle, Running, Complete, Failed };
  struct Network {
    char ssid[33] = {};
    int32_t rssi = 0;
    bool secured = false;
  };
  Network networks[maxNetworks];
  unsigned count = 0;
  State state = State::Idle;
  const char *error = "";

  bool running() const { return state == State::Running; }
  const char *stateName() const {
    switch (state) {
      case State::Running: return "running";
      case State::Complete: return "complete";
      case State::Failed: return "failed";
      default: return "idle";
    }
  }
  void begin(uint32_t now) {
    state = State::Running;
    startedMs_ = now;
    count = 0;
    error = "";
  }
  bool timedOut(uint32_t now) const {
    return running() && uint32_t(now - startedMs_) >= timeoutMs;
  }
  void fail(const char *why) { state = State::Failed; error = why; count = 0; }
  void complete() { state = State::Complete; }

  void add(const char *ssid, int32_t rssi, bool secured) {
    const unsigned length = strlen(ssid);
    if (!length || length > 32) return;
    unsigned position = 0;
    while (position < count && (strcmp(networks[position].ssid, ssid)
                               || networks[position].secured != secured)) ++position;
    if (position < count) {
      if (rssi <= networks[position].rssi) return;
    } else if (count < maxNetworks) ++count;
    else {
      position = count - 1;
      if (rssi <= networks[position].rssi) return;
    }
    Network &network = networks[position];
    memcpy(network.ssid, ssid, length + 1);
    network.rssi = rssi;
    network.secured = secured;
    while (position && networks[position].rssi > networks[position - 1].rssi) {
      const Network previous = networks[position - 1];
      networks[position - 1] = networks[position];
      networks[position--] = previous;
    }
  }

 private:
  uint32_t startedMs_ = 0;
};

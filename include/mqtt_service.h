#pragma once

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <IPAddress.h>
#include <lwip/dns.h>
#include "mqtt_commands.h"
#include "mqtt_discovery.h"
#include "target_move.h"

class DeskMqtt {
 public:
  DeskMqtt(DeskMonitor &desk, PanelBridge &panel, TargetMove &target)
      : desk_(desk), panel_(panel), target_(target) {}
  void begin(const char *id, const MqttSettings &settings);
  void configure(const MqttSettings &settings);
  void service(bool networkReady, bool allowConnect = true);
  bool connected() const { return announced_ && client_.connected(); }
  const char *stateName() const;
  const char *error() const { return error_; }
  const char *commandError() const { return commandError_; }
  uint32_t accepted() const { return accepted_; }
  uint32_t rejected() const { return inbox_.rejected + rejected_; }

 private:
  enum class Purpose { None, InitialState, Availability, Discovery, State };
  DeskMonitor &desk_;
  PanelBridge &panel_;
  TargetMove &target_;
  AsyncMqttClient client_;
  MqttSettings live_, pending_;
  MqttInbox inbox_;
  char id_[41] = {}, stateTopic_[128] = {}, availabilityTopic_[128] = {}, commandTopic_[128] = {};
  char state_[768] = {}, previousState_[768] = {}, discovery_[1280] = {};
  bool applying_ = false, closing_ = false, networkReady_ = false;
  bool connecting_ = false, announced_ = false, attempted_ = false;
  bool connectEvent_ = false, disconnectEvent_ = false, ackEvent_ = false, subscribeFailed_ = false;
  bool initialStateDone_ = false, stateDirty_ = true, hasHeight_ = false;
  uint8_t disconnectReason_ = 0, subscriptions_ = 0;
  uint16_t commandSubscription_ = 0, birthSubscription_ = 0, waitingPacket_ = 0;
  Purpose purpose_ = Purpose::None;
  unsigned discoveryNext_ = 0;
  uint32_t lastAttemptMs_ = 0, stageMs_ = 0, closeMs_ = 0, sentMs_ = 0;
  uint32_t lastStateCheckMs_ = 0, lastStateSentMs_ = 0, height_ = 0, manualId_ = 0;
  uint32_t accepted_ = 0, rejected_ = 0, seenRejected_ = 0;
  const char *error_ = "", *commandError_ = "", *lastCommand_ = "none";
  IPAddress resolvedAddress_;
  bool tcpStarted_ = false, dnsPending_ = false, dnsReady_ = false, dnsFailed_ = false;
  uint32_t generation_ = 0, dnsGeneration_ = 0;

  void setupClient();
  bool resolveAddress();
  static void dnsResult(const char *, const ip_addr_t *address, void *argument);
  void reset(uint32_t now);
  void fail(const char *why, uint32_t now);
  void execute(const MqttCommand &command, uint32_t now);
  void buildState(uint32_t now);
  bool publish(const char *topic, const char *payload, Purpose purpose, uint32_t now);
};

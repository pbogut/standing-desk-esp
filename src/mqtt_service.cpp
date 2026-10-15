#include "mqtt_service.h"

void DeskMqtt::begin(const char *id, const MqttSettings &settings) {
  strncpy(id_, id, sizeof(id_) - 1);
  live_ = settings;
  client_.onConnect([this](bool) { connectEvent_ = true; });
  client_.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
    disconnectReason_ = static_cast<uint8_t>(reason);
    disconnectEvent_ = true;
  });
  client_.onSubscribe([this](uint16_t id, uint8_t qos) {
    if (qos == 0x80) { subscribeFailed_ = true; return; }
    if (id == commandSubscription_) subscriptions_ |= 1;
    if (id == birthSubscription_) subscriptions_ |= 2;
  });
  client_.onPublish([this](uint16_t id) { if (id == waitingPacket_) ackEvent_ = true; });
  client_.onMessage([this](char *topic, char *payload, AsyncMqttClientMessageProperties properties,
                          size_t len, size_t index, size_t total) {
    const MqttCommandKind kind = mqttCommandKind(topic, live_.prefix);
    if (kind != MqttCommandKind::Birth && (!announced_ || applying_)) {
      ++rejected_; commandError_ = "mqtt_not_ready"; stateDirty_ = true; return;
    }
    inbox_.receive(kind, properties.retain, properties.dup, payload, len, index, total,
                   millis(), panel_.inputInterruptions);
  });
  setupClient();
}

void DeskMqtt::setupClient() {
  snprintf(stateTopic_, sizeof(stateTopic_), "%s/state", live_.prefix);
  snprintf(availabilityTopic_, sizeof(availabilityTopic_), "%s/availability", live_.prefix);
  snprintf(commandTopic_, sizeof(commandTopic_), "%s/command/+", live_.prefix);
  client_.setClientId(id_).setServer(live_.host, live_.port).setCleanSession(true)
      .setKeepAlive(15).setMaxTopicLength(160)
      .setCredentials(live_.username[0] ? live_.username : nullptr,
                      live_.username[0] ? live_.password : nullptr)
      .setWill(availabilityTopic_, 1, true, "offline");
}

void DeskMqtt::configure(const MqttSettings &settings) {
  pending_ = settings;
  applying_ = true;
  closing_ = false;
  inbox_.clear();
}

const char *DeskMqtt::stateName() const {
  if (applying_) return "applying";
  if (!live_.enabled) return "disabled";
  if (!networkReady_) return "waiting_wifi";
  if (connecting_) return "connecting";
  if (connected()) return "connected";
  if (client_.connected()) return "initializing";
  return "retry_wait";
}

void DeskMqtt::reset(uint32_t now) {
  if (manualId_) panel_.release(manualId_, now);
  manualId_ = 0;
  client_.disconnect(true); // An unclean disconnect lets the broker publish our offline will.
  client_.clearQueue();
  inbox_.clear();
  connecting_ = announced_ = connectEvent_ = ackEvent_ = false;
  subscriptions_ = 0;
  subscribeFailed_ = false;
  waitingPacket_ = 0;
  purpose_ = Purpose::None;
  attempted_ = true;
  lastAttemptMs_ = now;
}

void DeskMqtt::fail(const char *why, uint32_t now) {
  error_ = why;
  reset(now);
}

void DeskMqtt::execute(const MqttCommand &command, uint32_t now) {
  const char *failure = nullptr;
  switch (command.kind) {
    case MqttCommandKind::Move:
      lastCommand_ = command.value == 0x20 ? "up" : "down";
      if (!panel_.start(command.value, true, now)) failure = "panel_busy_or_release_pending";
      else manualId_ = panel_.commandId;
      break;
    case MqttCommandKind::Preset:
      lastCommand_ = "preset";
      if (!panel_.start(command.value, false, now)) failure = "panel_busy_or_release_pending";
      break;
    case MqttCommandKind::Target:
      lastCommand_ = "target";
      failure = target_.start(command.value, desk_, panel_, now);
      break;
    case MqttCommandKind::Cancel:
      lastCommand_ = "cancel";
      target_.stop("cancelled", panel_, now);
      break;
    default: failure = "unsupported_command";
  }
  if (failure) { commandError_ = failure; ++rejected_; }
  else { commandError_ = ""; ++accepted_; }
  stateDirty_ = true;
}

void DeskMqtt::buildState(uint32_t now) {
  char height[24] = "null", goal[16] = "null", failure[180] = "null";
  if (hasHeight_) snprintf(height, sizeof(height), "%lu.%02lu",
      static_cast<unsigned long>(height_ / 100), static_cast<unsigned long>(height_ % 100));
  if (target_.state != TargetMove::State::Idle)
    snprintf(goal, sizeof(goal), "%u", target_.targetHundredths / 100);
  // Command errors are firmware-owned literals, not user-provided payloads.
  if (commandError_[0]) snprintf(failure, sizeof(failure), "\"%s\"", commandError_);
  snprintf(state_, sizeof(state_),
      "{\"height_cm\":%s,\"height_fresh\":%s,\"target_height_cm\":%s,"
      "\"target_state\":\"%s\",\"target_reason\":\"%s\",\"target_ready\":%s,"
      "\"remote_button\":%u,\"last_command\":\"%s\",\"command_error\":%s,"
      "\"commands_accepted\":%lu,\"commands_rejected\":%lu}",
      height, desk_.automaticHeightFresh(now) ? "true" : "false", goal,
      target_.stateName(), target_.reason,
      !target_.active() && desk_.automaticHeightFresh(now) && panel_.targetReady(now) ? "true" : "false",
      panel_.commandButton, lastCommand_, failure,
      static_cast<unsigned long>(accepted_), static_cast<unsigned long>(rejected()));
}

bool DeskMqtt::publish(const char *topic, const char *payload, Purpose purpose, uint32_t now) {
  waitingPacket_ = client_.publish(topic, 1, true, payload);
  if (!waitingPacket_) { fail("publish_failed", now); return false; }
  purpose_ = purpose;
  sentMs_ = now;
  ackEvent_ = false;
  return true;
}

void DeskMqtt::service(bool networkReady, bool allowConnect) {
  const uint32_t now = millis();
  networkReady_ = networkReady;
  if (desk_.automaticHeightFresh(now)) { hasHeight_ = true; height_ = desk_.heightHundredths; }
  if (applying_) {
    if (!closing_) { reset(now); closing_ = true; closeMs_ = now; return; }
    if (uint32_t(now - closeMs_) < 100) return;
    live_ = pending_;
    setupClient();
    applying_ = closing_ = disconnectEvent_ = false;
    attempted_ = false;
    error_ = "";
  }
  if (disconnectEvent_) {
    disconnectEvent_ = false;
    const char *reason = disconnectReason_ == 4 ? "bad_credentials"
        : disconnectReason_ == 5 ? "not_authorized"
        : disconnectReason_ == 3 ? "broker_unavailable" : "broker_unreachable_or_disconnected";
    if (!error_[0] || disconnectReason_) error_ = reason;
    reset(now);
    disconnectEvent_ = false;
  }
  if (!live_.enabled || !networkReady) {
    if (client_.connected() || connecting_) reset(now);
    return;
  }
  if (connectEvent_) {
    connectEvent_ = false;
    if (!connecting_ || !client_.connected()) { reset(now); return; }
    connecting_ = false;
    error_ = "";
    stageMs_ = now;
    subscriptions_ = 0;
    commandSubscription_ = client_.subscribe(commandTopic_, 0);
    birthSubscription_ = client_.subscribe("homeassistant/status", 0);
    initialStateDone_ = announced_ = false;
    discoveryNext_ = 0;
    if (!commandSubscription_ || !birthSubscription_) { fail("subscribe_failed", now); return; }
  }
  if (!client_.connected()) {
    if (connecting_) {
      if (uint32_t(now - stageMs_) >= 10000) fail("connect_timeout", now);
    } else if (allowConnect && (!attempted_ || uint32_t(now - lastAttemptMs_) >= 5000)) {
      attempted_ = connecting_ = true;
      lastAttemptMs_ = stageMs_ = now;
      client_.clearQueue();
      client_.connect();
    }
    return;
  }
  if (subscribeFailed_) { fail("subscription_denied", now); return; }
  if (subscriptions_ != 3) {
    if (uint32_t(now - stageMs_) >= 10000) fail("subscribe_timeout", now);
    return;
  }
  if (announced_) {
    MqttCommand command;
    if (inbox_.take(command, now, panel_.inputInterruptions)) execute(command, now);
    if (inbox_.rejected != seenRejected_) {
      seenRejected_ = inbox_.rejected;
      commandError_ = inbox_.error;
      stateDirty_ = true;
    }
  }
  if (waitingPacket_) {
    if (!ackEvent_) {
      if (uint32_t(now - sentMs_) >= 10000) fail("publish_timeout", now);
      return;
    }
    if (purpose_ == Purpose::InitialState) initialStateDone_ = true;
    else if (purpose_ == Purpose::Availability) announced_ = true;
    else if (purpose_ == Purpose::Discovery) ++discoveryNext_;
    waitingPacket_ = 0;
    ackEvent_ = false;
  }
  if (!initialStateDone_) {
    buildState(now);
    publish(stateTopic_, state_, Purpose::InitialState, now);
    return;
  }
  if (!announced_) { publish(availabilityTopic_, "online", Purpose::Availability, now); return; }
  if (inbox_.birthRequested) { discoveryNext_ = 0; inbox_.birthRequested = false; }
  if (discoveryNext_ < mqttEntityCount) {
    char topic[160];
    if (!mqttDiscoveryTopic(id_, discoveryNext_, topic, sizeof(topic))
        || !mqttDiscoveryPayload(id_, live_.prefix, discoveryNext_, discovery_, sizeof(discovery_))) {
      fail("discovery_failed", now); return;
    }
    publish(topic, discovery_, Purpose::Discovery, now);
    return;
  }
  if (stateDirty_ || uint32_t(now - lastStateCheckMs_) >= 250) {
    stateDirty_ = false;
    lastStateCheckMs_ = now;
    buildState(now);
    if (strcmp(state_, previousState_) || uint32_t(now - lastStateSentMs_) >= 15000) {
      if (publish(stateTopic_, state_, Purpose::State, now)) {
        strcpy(previousState_, state_);
        lastStateSentMs_ = now;
      }
    }
  }
}

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class MqttCommandKind { Invalid, Preset, Move, Target, Cancel, Birth };

inline MqttCommandKind mqttCommandKind(const char *topic, const char *prefix) {
  if (!strcmp(topic, "homeassistant/status")) return MqttCommandKind::Birth;
  const unsigned length = strlen(prefix);
  if (strncmp(topic, prefix, length) || strncmp(topic + length, "/command/", 9))
    return MqttCommandKind::Invalid;
  const char *name = topic + length + 9;
  if (!strcmp(name, "preset")) return MqttCommandKind::Preset;
  if (!strcmp(name, "move")) return MqttCommandKind::Move;
  if (!strcmp(name, "target")) return MqttCommandKind::Target;
  if (!strcmp(name, "cancel")) return MqttCommandKind::Cancel;
  return MqttCommandKind::Invalid;
}

struct MqttCommand {
  MqttCommandKind kind = MqttCommandKind::Invalid;
  uint16_t value = 0;
  uint32_t receivedMs = 0, panelSequence = 0;
};

class MqttInbox {
 public:
  static constexpr uint32_t maxAgeMs = 250, assemblyTimeoutMs = 500;
  uint32_t rejected = 0;
  const char *error = "";
  bool birthRequested = false;

  void clear() { pending_ = assembling_ = false; birthRequested = false; }
  void receive(MqttCommandKind kind, bool retained, bool duplicate, const char *payload,
               size_t len, size_t index, size_t total, uint32_t now, uint32_t sequence) {
    if (index == 0) {
      assembling_ = false;
      if (kind == MqttCommandKind::Invalid) { reject("unsupported_topic"); return; }
      if (kind != MqttCommandKind::Birth && (retained || duplicate)) {
        reject(retained ? "retained_command" : "duplicate_command"); return;
      }
      if (!total || total >= sizeof(payload_)) { reject("invalid_payload"); return; }
      kind_ = kind;
      used_ = 0;
      total_ = total;
      startedMs_ = now;
      sequence_ = sequence;
      assembling_ = true;
    }
    if (!assembling_) return;
    if (kind != kind_ || index != used_ || total != total_ || len > total_ - used_
        || uint32_t(now - startedMs_) >= assemblyTimeoutMs
        || (kind != MqttCommandKind::Birth && (retained || duplicate))) {
      assembling_ = false; reject("invalid_fragments"); return;
    }
    memcpy(payload_ + used_, payload, len);
    used_ += len;
    if (used_ != total_) return;
    assembling_ = false;
    if (memchr(payload_, 0, total_)) { reject("invalid_payload"); return; }
    payload_[total_] = 0;
    if (kind_ == MqttCommandKind::Birth) {
      if (!strcmp(payload_, "online")) birthRequested = true;
      return;
    }
    MqttCommand command;
    command.kind = kind_;
    command.receivedMs = now;
    command.panelSequence = sequence_;
    if (kind_ == MqttCommandKind::Move) {
      if (!strcmp(payload_, "up")) command.value = 0x20;
      else if (!strcmp(payload_, "down")) command.value = 0x40;
      else { reject("invalid_payload"); return; }
    } else if (kind_ == MqttCommandKind::Preset) {
      if (total_ != 1 || payload_[0] < '1' || payload_[0] > '3') {
        reject("invalid_payload"); return;
      }
      command.value = 1 << (payload_[0] - '0');
    } else if (kind_ == MqttCommandKind::Target) {
      unsigned length = total_;
      if (length >= 3 && payload_[length - 2] == '.' && payload_[length - 1] == '0') length -= 2;
      if (!length || length > 3) { reject("invalid_payload"); return; }
      for (unsigned i = 0; i < length; ++i) {
        if (payload_[i] < '0' || payload_[i] > '9') { reject("invalid_payload"); return; }
        command.value = command.value * 10 + payload_[i] - '0';
      }
      if (command.value < 68 || command.value > 120) { reject("invalid_payload"); return; }
    } else if (strcmp(payload_, "cancel")) { reject("invalid_payload"); return; }
    if (pending_ && kind_ != MqttCommandKind::Cancel) { reject("command_pending"); return; }
    command_ = command;
    pending_ = true;
  }

  bool take(MqttCommand &command, uint32_t now, uint32_t sequence) {
    if (!pending_) return false;
    pending_ = false;
    if (uint32_t(now - command_.receivedMs) >= maxAgeMs) { reject("stale_command"); return false; }
    if (sequence != command_.panelSequence) { reject("panel_priority"); return false; }
    command = command_;
    return true;
  }

 private:
  bool pending_ = false, assembling_ = false;
  MqttCommand command_;
  MqttCommandKind kind_ = MqttCommandKind::Invalid;
  char payload_[17] = {};
  size_t used_ = 0, total_ = 0;
  uint32_t startedMs_ = 0, sequence_ = 0;
  void reject(const char *why) { ++rejected; error = why; }
};

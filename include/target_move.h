#pragma once

#include "desk_protocol.h"
#include "panel_bridge.h"

class TargetMove {
 public:
  static constexpr uint32_t progressTimeoutMs = 2000;
  enum class State { Idle, Moving, Complete, Cancelled };
  State state = State::Idle;
  const char *reason = "idle";
  uint16_t targetHundredths = 0;
  uint32_t commandId = 0;
  uint8_t button = 0;

  bool active() const { return state == State::Moving; }

  const char *stateName() const {
    switch (state) {
      case State::Moving: return "moving";
      case State::Complete: return "complete";
      case State::Cancelled: return "cancelled";
      default: return "idle";
    }
  }

  const char *start(uint16_t heightCm, const DeskMonitor &desk,
                    PanelBridge &panel, uint32_t now) {
    if (heightCm < 68 || heightCm > 120) return "height_cm must be between 68 and 120";
    if (active()) return "An automatic move is already active";
    if (!desk.automaticHeightFresh(now)) return "A fresh clean height display is required";
    if (!panel.ready(now)) return "Panel must be idle; command busy or release gap active";
    const uint16_t requested = heightCm * 100;
    if (desk.heightHundredths == requested) {
      targetHundredths = requested;
      commandId = panel.commandId;
      button = 0;
      state = State::Complete;
      reason = "reached";
      return nullptr;
    }
    const uint8_t direction = requested > desk.heightHundredths ? 0x20 : 0x40;
    if (!panel.startTarget(direction, now)) return "Panel command could not start";
    targetHundredths = requested;
    commandId = panel.commandId;
    button = direction;
    startedMs_ = progressMs_ = now;
    lastHeight_ = desk.heightHundredths;
    state = State::Moving;
    reason = "moving";
    return nullptr;
  }

  void update(const DeskMonitor &desk, PanelBridge &panel, uint32_t now) {
    if (!active()) return;
    if (panel.commandId != commandId || panel.commandButton != button) {
      stop(panel.stopReason, panel, now);
      return;
    }
    if (!desk.displayHeight) { stop("invalid_display", panel, now); return; }
    if (!desk.automaticHeightFresh(now)) { stop("height_stale", panel, now); return; }
    if (uint32_t(now - startedMs_) >= PanelBridge::maxTargetMs) {
      stop("travel_timeout", panel, now);
      return;
    }
    const uint32_t height = desk.heightHundredths;
    if ((button == 0x20 && height < lastHeight_)
        || (button == 0x40 && height > lastHeight_)) {
      stop("wrong_direction", panel, now);
      return;
    }
    if ((button == 0x20 && height >= targetHundredths)
        || (button == 0x40 && height <= targetHundredths)) {
      panel.release(commandId, now);
      state = State::Complete;
      reason = "reached";
      return;
    }
    if (height != lastHeight_) {
      lastHeight_ = height;
      progressMs_ = now;
    }
    if (uint32_t(now - progressMs_) >= progressTimeoutMs) {
      stop("no_progress", panel, now);
      return;
    }
    if (!panel.renewTarget(commandId, button, now)) stop(panel.stopReason, panel, now);
  }

  void stop(const char *why, PanelBridge &panel, uint32_t now) {
    if (!active()) return;
    panel.release(commandId, now);
    state = State::Cancelled;
    reason = why;
  }

  bool cancel(uint32_t id, PanelBridge &panel, uint32_t now) {
    if (!id || id != commandId) return false;
    stop("cancelled", panel, now);
    return true;
  }

 private:
  uint32_t startedMs_ = 0, progressMs_ = 0, lastHeight_ = 0;
};

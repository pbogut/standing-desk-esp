#pragma once

#include <stdint.h>
#include <string.h>

class PanelBridge {
 public:
  static constexpr unsigned bytesPerPass = 8;
  static constexpr uint32_t staleMs = 1000;
  static constexpr uint32_t frameGapMs = 30, panelTimeoutMs = 250;
  static constexpr uint32_t commandIntervalMs = 10, presetMs = 100;
  static constexpr uint32_t holdLeaseMs = 300, maxHoldMs = 10000;
  static constexpr uint32_t maxTargetMs = 60000;
  static constexpr uint32_t releaseGapMs = 100;
  uint32_t bytesReceived = 0, bytesForwarded = 0, writeFailures = 0;
  uint32_t bytesSuppressed = 0, espBytesSent = 0;
  uint32_t invalidFrames = 0, partialTimeouts = 0, physicalOverrides = 0;
  uint32_t inputInterruptions = 0;
  uint32_t lastByteMs = 0;
  uint32_t commandId = 0;
  uint8_t commandButton = 0;
  const char *stopReason = "idle";

  bool fresh(uint32_t now) const {
    return bytesReceived != 0 && uint32_t(now - lastByteMs) < staleMs;
  }

  bool ready(uint32_t now) const {
    // A sleeping panel is silent. Recent presses/noise still block new commands.
    return commandFree(now) && ((panelIdle_
        && uint32_t(now - lastFrameMs_) < panelTimeoutMs)
        || bytesReceived == 0 || uint32_t(now - lastByteMs) >= panelTimeoutMs);
  }

  bool targetReady(uint32_t now) const {
    return commandFree(now) && panelIdle_
        && uint32_t(now - lastFrameMs_) < panelTimeoutMs;
  }

  bool start(uint8_t button, bool hold, uint32_t now) {
    expire(now);
    if (!ready(now)) return false;
    if (hold ? button != 0x20 && button != 0x40
             : button != 0x02 && button != 0x04 && button != 0x08) return false;
    if (++commandId == 0) ++commandId;
    commandButton = button;
    hold_ = hold;
    target_ = false;
    stopReason = "active";
    startedMs_ = renewedMs_ = now;
    pressSent_ = false;
    return true;
  }

  bool renew(uint32_t id, uint8_t button, uint32_t now) {
    return renewPress(id, button, false, now);
  }

  bool startTarget(uint8_t button, uint32_t now) {
    expire(now);
    if (!targetReady(now)) return false;
    if (!start(button, true, now)) return false;
    target_ = true;
    return true;
  }

  bool renewTarget(uint32_t id, uint8_t button, uint32_t now) {
    return renewPress(id, button, true, now);
  }

  bool release(uint32_t id, uint32_t now) {
    if (id == 0 || id != commandId) return false;
    cancel(now, "released");
    return true;
  }

  void inputOverflow(uint32_t now) {
    ++inputInterruptions;
    panelIdle_ = false;
    cancel(now, "panel_uart_overflow");
  }

 private:
  bool commandFree(uint32_t now) const {
    return commandButton == 0 && !releasePending_
        && (!hasEnded_ || uint32_t(now - endedMs_) >= releaseGapMs);
  }

  bool renewPress(uint32_t id, uint8_t button, bool target, uint32_t now) {
    expire(now);
    if (!commandButton || !hold_ || target != target_ || id != commandId
        || button != commandButton)
      return false;
    renewedMs_ = now;
    return true;
  }

 public:
  template <typename SerialPort>
  void forward(SerialPort &port, uint32_t now) {
    expire(now);
    if (used_ && uint32_t(now - lastByteMs) >= frameGapMs) {
      ++partialTimeouts;
      ++inputInterruptions;
      panelIdle_ = aligned_ = false;
      cancel(now, "partial_panel_frame");
      writeBytes(port, frame_, used_, false);
      used_ = 0;
    }
    // Bound RX processing; never insert a command inside a panel frame.
    for (unsigned count = 0; count < bytesPerPass && port.available(); ++count) {
      const int byte = port.read();
      if (byte < 0) break;
      ++bytesReceived;
      lastByteMs = now;
      if (!used_ && byte != 0xA5) {
        ++inputInterruptions;
        panelIdle_ = aligned_ = false;
        cancel(now, "invalid_panel_frame");
        const uint8_t raw = static_cast<uint8_t>(byte);
        writeBytes(port, &raw, 1, false);
        continue;
      }
      frame_[used_++] = static_cast<uint8_t>(byte);
      if (used_ == sizeof(frame_)) processFrame(port, now);
    }
    // Drain queued physical traffic first. A receive gap also permits release
    // after malformed/truncated traffic, without inserting into its payload.
    if (port.available() || used_ || (bytesReceived && !aligned_
        && uint32_t(now - lastByteMs) < frameGapMs)) return;
    if (releasePending_) {
      if (uint32_t(now - lastTxMs_) >= commandIntervalMs) {
        releasePending_ = !writeCommand(port, 0, now);
        if (!releasePending_) endedMs_ = now;
      }
    } else if (commandButton && (!pressSent_
               || uint32_t(now - lastTxMs_) >= commandIntervalMs)) {
      pressSent_ = true;
      if (!writeCommand(port, commandButton, now)) cancel(now, "write_failure");
    }
  }

 private:
  uint8_t frame_[5] = {}, used_ = 0;
  bool panelIdle_ = false, aligned_ = false, hold_ = false;
  bool target_ = false;
  bool pressSent_ = false, releasePending_ = false, hasEnded_ = false;
  uint32_t lastFrameMs_ = 0, lastTxMs_ = 0;
  uint32_t startedMs_ = 0, renewedMs_ = 0, endedMs_ = 0;

  void cancel(uint32_t now, const char *reason) {
    if (!commandButton) return;
    releasePending_ = pressSent_;
    commandButton = 0;
    stopReason = reason;
    endedMs_ = now;
    hasEnded_ = true;
  }

  void expire(uint32_t now) {
    if (!commandButton) return;
    if (target_ && (!panelIdle_ || uint32_t(now - lastFrameMs_) >= panelTimeoutMs))
      cancel(now, "panel_stale");
    else if (hold_ && uint32_t(now - renewedMs_) >= holdLeaseMs)
      cancel(now, "lease_expired");
    else if (hold_ && uint32_t(now - startedMs_) >= (target_ ? maxTargetMs : maxHoldMs))
      cancel(now, "travel_timeout");
    else if (!hold_ && uint32_t(now - startedMs_) >= presetMs)
      cancel(now, "released");
  }

  template <typename SerialPort>
  bool writeBytes(SerialPort &port, const uint8_t *data, unsigned size, bool esp) {
    bool ok = true;
    for (unsigned i = 0; i < size; ++i) {
      if (port.write(data[i]) == 1) {
        if (esp) ++espBytesSent;
        else ++bytesForwarded;
      } else {
        ++writeFailures;
        ok = false;
      }
    }
    return ok;
  }

  template <typename SerialPort>
  bool writeCommand(SerialPort &port, uint8_t button, uint32_t now) {
    const uint8_t command[] = {0xA5, 0, button, 0, button};
    lastTxMs_ = now;
    return writeBytes(port, command, sizeof(command), true);
  }

  template <typename SerialPort>
  void processFrame(SerialPort &port, uint32_t now) {
    const uint8_t sum = frame_[1] + frame_[2] + frame_[3];
    if (sum != frame_[4]) {
      ++invalidFrames;
      ++inputInterruptions;
      panelIdle_ = aligned_ = false;
      cancel(now, "invalid_panel_frame");
      // Forward corrupt bytes unchanged, retaining an embedded candidate header.
      unsigned next = 1;
      while (next < used_ && frame_[next] != 0xA5) ++next;
      writeBytes(port, frame_, next, false);
      used_ -= next;
      memmove(frame_, frame_ + next, used_);
      return;
    }
    aligned_ = true;
    lastFrameMs_ = now;
    panelIdle_ = frame_[1] == 0 && frame_[2] == 0 && frame_[3] == 0;
    if (!panelIdle_) {
      ++inputInterruptions;
      if (commandButton) ++physicalOverrides;
      cancel(now, "panel_override");
      // A release before takeover makes even the same preset a new press.
      if (releasePending_) {
        releasePending_ = !writeCommand(port, 0, now);
        if (!releasePending_) endedMs_ = now;
      }
    }
    if (panelIdle_ && commandButton) {
      bytesSuppressed += sizeof(frame_);
    } else {
      const bool ok = writeBytes(port, frame_, sizeof(frame_), false);
      if (panelIdle_ && ok && releasePending_) {
        releasePending_ = false;
        endedMs_ = now;
      }
      if (!ok) cancel(now, "write_failure");
    }
    used_ = 0;
  }
};

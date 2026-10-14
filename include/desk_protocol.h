#pragma once

#include <stdint.h>
#include <string.h>

class DeskMonitor {
 public:
  static constexpr uint32_t staleMs = 1000;
  static constexpr uint32_t frameGapMs = 30;
  uint32_t bytes = 0, validFrames = 0, checksumFailures = 0;
  uint32_t invalidDisplays = 0, partialTimeouts = 0;
  uint8_t lastFrame[6] = {};
  bool hasFrame = false, hasHeight = false, displayNumeric = false;
  bool displayHeight = false;
  uint32_t lastFrameMs = 0, lastHeightMs = 0;
  uint32_t heightHundredths = 0;

  bool fresh(uint32_t now) const {
    return hasHeight && displayNumeric && uint32_t(now - lastHeightMs) < staleMs;
  }

  bool automaticHeightFresh(uint32_t now) const {
    return displayHeight && hasHeight && uint32_t(now - lastHeightMs) < 250;
  }

  void inputOverflow() { displayHeight = false; }

  void feed(uint8_t byte, uint32_t now) {
    ++bytes;
    if (used_ && uint32_t(now - lastByteMs_) >= frameGapMs) {
      displayHeight = false;
      used_ = 0;
      ++partialTimeouts;
    }
    lastByteMs_ = now;
    if (!used_ && byte != 0x5A) return;
    buffer_[used_++] = byte;
    if (used_ != sizeof(buffer_)) return;
    memcpy(lastFrame, buffer_, sizeof(buffer_));
    hasFrame = true;
    lastFrameMs = now;
    const uint8_t sum = buffer_[1] + buffer_[2] + buffer_[3] + buffer_[4];
    if (sum != buffer_[5]) {
      displayHeight = false;
      ++checksumFailures;
      // Preserve a possible header inside a damaged frame.
      uint8_t next = 1;
      while (next < used_ && buffer_[next] != 0x5A) ++next;
      used_ -= next;
      memmove(buffer_, buffer_ + next, used_);
      return;
    }
    ++validFrames;
    displayHeight = false;
    uint16_t value = 0;
    int decimal = -1;
    displayNumeric = true;
    for (int i = 0; i < 3; ++i) {
      const int digit = decodeDigit(buffer_[i + 1] & 0x7F);
      if (digit < 0) displayNumeric = false;
      else value = value * 10 + digit;
      if (buffer_[i + 1] & 0x80) {
        if (decimal != -1 || i == 2) displayNumeric = false;
        decimal = i;
      }
    }
    used_ = 0;
    if (!displayNumeric) {
      ++invalidDisplays;
      return;
    }
    heightHundredths = value * (decimal < 0 ? 100 : decimal == 1 ? 10 : 1);
    displayHeight = (decimal == 1 && heightHundredths >= 6800
                     && heightHundredths < 10000)
        || (decimal == -1 && heightHundredths >= 10000
            && heightHundredths <= 12000);
    hasHeight = true;
    lastHeightMs = now;
  }

 private:
  static int decodeDigit(uint8_t code) {
    static const uint8_t codes[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                    0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    for (int i = 0; i < 10; ++i) if (codes[i] == code) return i;
    return -1;
  }
  uint8_t buffer_[6] = {}, used_ = 0;
  uint32_t lastByteMs_ = 0;
};

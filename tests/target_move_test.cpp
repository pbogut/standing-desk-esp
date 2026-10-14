#include "target_move.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

struct FakeSerial {
  std::vector<uint8_t> input, output;
  unsigned position = 0;
  bool failWrite = false;
  int available() const { return input.size() - position; }
  int read() { return available() ? input[position++] : -1; }
  unsigned write(uint8_t byte) {
    if (failWrite) return 0;
    output.push_back(byte);
    return 1;
  }
};

static std::vector<uint8_t> display(uint16_t hundredths) {
  const uint8_t codes[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                           0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const bool decimal = hundredths < 10000;
  const unsigned value = hundredths / (decimal ? 10 : 100);
  std::vector<uint8_t> frame = {0x5A, codes[value / 100], codes[value / 10 % 10],
                               codes[value % 10], 0x10, 0};
  if (decimal) frame[2] |= 0x80;
  frame[5] = frame[1] + frame[2] + frame[3] + frame[4];
  return frame;
}

struct Fixture {
  DeskMonitor desk;
  PanelBridge panel;
  TargetMove target;
  FakeSerial serial;
  uint32_t now = 0;

  void observe(const std::vector<uint8_t> &frame) {
    for (uint8_t byte : frame) {
      desk.feed(byte, now);
      target.update(desk, panel, now);
    }
  }
  void panelFrame(uint8_t button = 0) {
    serial.input.insert(serial.input.end(), {0xA5, 0, button, 0, button});
    while (serial.available()) panel.forward(serial, now);
    target.update(desk, panel, now);
  }
  void tick(uint16_t height, uint32_t time) {
    now = time;
    observe(display(height));
    panelFrame();
  }
  void start(uint16_t height, uint16_t goal, uint32_t time = 0) {
    tick(height, time);
    assert(target.start(goal, desk, panel, now) == nullptr);
    panel.forward(serial, now);
    serial.output.clear();
  }
  void stopped(const char *reason) const {
    assert(!target.active() && panel.commandButton == 0);
    assert(strcmp(target.reason, reason) == 0);
  }
};

int main() {
  // Only the observed decimal/integer height layouts within 68..120 qualify.
  {
    Fixture f;
    for (uint16_t height : {6800, 7860, 9990, 10000, 10500, 12000}) {
      f.tick(height, 0);
      assert(f.desk.automaticHeightFresh(0));
      assert(!f.desk.automaticHeightFresh(250));
    }
    for (uint16_t height : {6790, 12100}) {
      f.tick(height, 0);
      assert(f.desk.fresh(0) && !f.desk.automaticHeightFresh(0));
    }
    // Integer 078, decimal 105.0, and first-digit decimal are numeric but not height.
    for (const auto &frame : std::vector<std::vector<uint8_t>>{
             {0x5A, 0x3F, 0x07, 0x7F, 0x10, 0xD5},
             {0x5A, 0x06, 0xBF, 0x6D, 0x10, 0x42},
             {0x5A, 0x87, 0x7F, 0x7D, 0x10, 0x93}}) {
      f.observe(frame);
      assert(f.desk.fresh(0) && !f.desk.automaticHeightFresh(0));
    }
  }

  // Reject absent, unsupported, stale, busy, or out-of-range starts with no TX.
  {
    Fixture f;
    assert(f.target.start(80, f.desk, f.panel, 0) != nullptr);
    f.tick(7000, 0);
    const auto output = f.serial.output;
    assert(f.target.start(67, f.desk, f.panel, 0) != nullptr);
    assert(f.target.start(121, f.desk, f.panel, 0) != nullptr);
    assert(f.target.start(80, f.desk, f.panel, 250) != nullptr);
    f.now = 0;
    f.panelFrame(0x04);
    assert(f.target.start(80, f.desk, f.panel, 0) != nullptr);
    assert(f.panel.commandButton == 0);
    assert(f.serial.output.size() == output.size() + 5);
  }

  // At target is a no-op. Both directions stop at or beyond target without reversing.
  for (bool up : {false, true}) {
    Fixture f;
    f.start(8000, 80);
    f.stopped("reached");
    assert(f.serial.output.empty());
    f.start(up ? 7900 : 8100, 80);
    const uint8_t direction = up ? 0x20 : 0x40;
    assert(f.panel.commandButton == direction);
    f.tick(up ? 7950 : 8050, 100);
    assert(f.target.active());
    f.tick(up ? 8010 : 7990, 200);
    f.stopped("reached");
    assert(f.target.state == TargetMove::State::Complete);
    assert(f.serial.output[f.serial.output.size() - 3] == 0);
    f.tick(8000, 300);
    assert(f.panel.commandButton == 0);
  }

  // Wrong-direction changes cancel immediately in either direction.
  for (bool up : {false, true}) {
    Fixture f;
    f.start(8000, up ? 90 : 70);
    f.tick(up ? 7990 : 8010, 100);
    f.stopped("wrong_direction");
  }

  // Display changes and corrupt checksums latch cancellation, even if a valid
  // height follows in the same batch. Cached numeric height never restarts motion.
  for (bool corrupt : {false, true}) {
    Fixture f;
    f.start(7000, 90);
    f.now = 10;
    auto frame = corrupt ? display(7010) : std::vector<uint8_t>{0x5A, 0, 0, 0, 0x10, 0x10};
    if (corrupt) ++frame[5];
    f.observe(frame);
    f.stopped("invalid_display");
    f.observe(display(7010));
    f.panelFrame();
    f.stopped("invalid_display");
    assert(f.serial.output[f.serial.output.size() - 3] == 0);
  }

  // A truncated status frame also invalidates automatic height on its receive gap.
  {
    Fixture f;
    f.start(7000, 90);
    f.now = 1;
    f.observe({0x5A, 0x07});
    f.now = 31;
    f.observe(display(7010));
    f.stopped("invalid_display");
  }

  // Missing height cancels at 250 ms, even with continuing idle panel traffic.
  {
    Fixture f;
    f.start(7000, 90);
    for (uint32_t time : {100, 200, 249}) {
      f.now = time;
      f.target.update(f.desk, f.panel, time);
      f.panelFrame();
      assert(f.target.active());
    }
    f.now = 250;
    f.target.update(f.desk, f.panel, f.now);
    f.stopped("height_stale");
    f.panelFrame();
    assert(f.serial.output[f.serial.output.size() - 3] == 0);
  }

  // Repeated equal readings are normal, but two seconds without progress cancels.
  for (uint16_t height : {7000, 10500}) {
    Fixture f;
    f.start(height, 120);
    for (uint32_t time = 100; time < 2000; time += 100) {
      f.tick(height, time);
      assert(f.target.active());
    }
    f.tick(height, 2000);
    f.stopped("no_progress");
  }

  // Real progress resets the stall timeout; target mode can run beyond ten seconds.
  {
    Fixture f;
    f.start(7000, 120);
    for (uint32_t time = 100; time < PanelBridge::maxTargetMs; time += 100) {
      f.tick(7000 + (time / 1000) * 10, time);
      assert(f.target.active());
      assert(!f.panel.renew(f.target.commandId, 0x20, time));
    }
    f.tick(7600, PanelBridge::maxTargetMs);
    f.stopped("travel_timeout");
  }

  // Physical buttons cancel target movement and pass through. Local renewals
  // cannot restart it, and browser hold renewals cannot renew an automatic move.
  for (uint8_t button : {0x02, 0x20, 0x40, 0x80}) {
    Fixture f;
    f.start(7000, 90);
    assert(!f.panel.renew(f.target.commandId, 0x20, 1));
    f.now = 10;
    f.panelFrame(button);
    f.stopped("panel_override");
    assert(f.serial.output[f.serial.output.size() - 3] == button);
    f.tick(7010, 110);
    f.stopped("panel_override");
  }

  // Explicit cancel/release, input overflow, and failed TX cancel without resuming.
  {
    Fixture f;
    f.start(7000, 90);
    const uint32_t oldId = f.target.commandId;
    assert(!f.target.cancel(oldId + 1, f.panel, 1));
    assert(f.target.cancel(oldId, f.panel, 1));
    f.stopped("cancelled");
    assert(f.target.cancel(oldId, f.panel, 2));
    f.tick(7000, 10);
    f.tick(7000, 110);
    assert(f.target.start(90, f.desk, f.panel, 110) == nullptr);
    assert(!f.target.cancel(oldId, f.panel, 111));
    assert(f.target.active());
    f.panel.release(f.target.commandId, 111);
    f.target.update(f.desk, f.panel, 111);
    f.stopped("released");
  }
  {
    Fixture f;
    f.start(7000, 90);
    f.desk.inputOverflow();
    f.target.update(f.desk, f.panel, 1);
    f.stopped("invalid_display");
  }
  {
    Fixture f;
    f.start(7000, 90);
    f.panel.inputOverflow(1);
    f.target.update(f.desk, f.panel, 1);
    f.stopped("panel_uart_overflow");
  }
  {
    Fixture f;
    f.start(7000, 90);
    f.serial.failWrite = true;
    f.panel.forward(f.serial, 10);
    f.target.update(f.desk, f.panel, 10);
    f.stopped("write_failure");
  }

  // Progress and target deadlines remain correct across millis wraparound.
  {
    Fixture f;
    const uint32_t start = UINT32_MAX - 1000;
    f.start(7000, 90, start);
    for (uint32_t elapsed = 100; elapsed < 2000; elapsed += 100) {
      f.tick(7000, start + elapsed);
      assert(f.target.active());
    }
    f.tick(7000, start + 2000);
    f.stopped("no_progress");
  }
  puts("All target move tests passed.");
}

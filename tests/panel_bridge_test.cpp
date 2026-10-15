#include "panel_bridge.h"

#include <assert.h>
#include <stdio.h>
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

static std::vector<uint8_t> packet(uint8_t button) {
  return {0xA5, 0, button, 0, button};
}

static void input(PanelBridge &bridge, FakeSerial &port,
                  const std::vector<uint8_t> &data, uint32_t now) {
  port.input.insert(port.input.end(), data.begin(), data.end());
  while (port.available()) bridge.forward(port, now);
}

static void idle(PanelBridge &bridge, FakeSerial &port, uint32_t now) {
  input(bridge, port, packet(0), now);
}

static void expectPackets(const std::vector<uint8_t> &bytes,
                          const std::vector<uint8_t> &buttons) {
  std::vector<uint8_t> expected;
  for (uint8_t button : buttons) {
    const auto frame = packet(button);
    expected.insert(expected.end(), frame.begin(), frame.end());
  }
  assert(bytes == expected);
}

int main() {
  PanelBridge bridge;
  FakeSerial port;
  // No synthetic idle or movement packets when the panel is absent.
  bridge.forward(port, 0);
  assert(port.output.empty() && !bridge.fresh(0));

  // Preserve idle, UP, release, and unknown/programming traffic byte for byte.
  port.input = {0xA5, 0, 0, 0, 0, 0xA5, 0, 0x20, 0, 0x20,
                0xA5, 0, 0, 0, 0, 0xA5, 0xFF, 0x81, 0x55, 0xD5};
  bridge.forward(port, 10);
  assert(port.position == PanelBridge::bytesPerPass);
  assert(port.available());
  while (port.available()) bridge.forward(port, 10);
  assert(port.output == port.input);
  assert(bridge.bytesReceived == port.input.size());
  assert(bridge.bytesForwarded == port.input.size());
  assert(bridge.writeFailures == 0);
  assert(bridge.fresh(1009) && !bridge.fresh(1010));
  bridge.forward(port, 2000);
  assert(port.output == port.input && bridge.lastByteMs == 10);

  // Transparent forwarding does not filter any byte value or require framing.
  FakeSerial allBytes;
  for (unsigned byte = 0; byte < 256; ++byte) allBytes.input.push_back(byte);
  while (allBytes.available()) bridge.forward(allBytes, UINT32_MAX - 500);
  bridge.forward(allBytes, UINT32_MAX - 470);
  assert(allBytes.output == allBytes.input);
  assert(bridge.fresh(498) && !bridge.fresh(499));

  FakeSerial failure;
  failure.input = {0xA5};
  failure.failWrite = true;
  const uint32_t forwarded = bridge.bytesForwarded;
  bridge.forward(failure, 1000);
  bridge.forward(failure, 1030);
  assert(bridge.writeFailures == 1 && bridge.bytesForwarded == forwarded);

  // Physical wake-up bytes pass immediately while idle, even if a packet is
  // short, corrupt, or split across loop iterations. Timeout never repeats them.
  {
    PanelBridge remote;
    FakeSerial serial;
    input(remote, serial, {0xA5}, 1000);
    assert((serial.output == std::vector<uint8_t>{0xA5}));
    input(remote, serial, {0, 0x20}, 1001);
    assert((serial.output == std::vector<uint8_t>{0xA5, 0, 0x20}));
    assert(remote.wakeCount == 3 && remote.wakeStartMs == 1000);
    assert(remote.wakeBytes[0] == 0xA5 && remote.wakeBytes[2] == 0x20);
    assert(remote.wakeReadOffsetsMs[0] == 0 && remote.wakeReadOffsetsMs[2] == 1);
    assert(remote.bufferedBytes() == 0);
    remote.forward(serial, 1031);
    assert((serial.output == std::vector<uint8_t>{0xA5, 0, 0x20}));
    assert(remote.partialTimeouts == 1);
    input(remote, serial, {0xA5, 0x99, 0xA5, 0, 0x40}, 1040);
    assert(serial.output == serial.input);
    input(remote, serial, {0, 0x40}, 1041);
    assert(serial.output == serial.input && remote.invalidFrames == 1);
    assert(remote.bytesReceived == remote.bytesForwarded);
    for (unsigned i = 0; i < 40; ++i) input(remote, serial, {0}, 1050);
    assert(remote.wakeCount == 32);
    input(remote, serial, {0xA5}, 2050);
    assert(remote.wakeCount == 1 && remote.wakeStartMs == 2050 && remote.wakeBytes[0] == 0xA5);
  }

  // A request arriving after a physical frame's prefix was forwarded must finish
  // that frame before injecting. It cannot suppress/duplicate its remaining bytes.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    input(remote, serial, {0xA5, 0}, 10);
    assert((serial.output == std::vector<uint8_t>{0xA5, 0}));
    assert(remote.start(0x02, false, 10));
    remote.forward(serial, 10);
    assert(serial.output.size() == 2);
    input(remote, serial, {0}, 11);
    assert((serial.output == std::vector<uint8_t>{0xA5, 0, 0}));
    input(remote, serial, {0, 0}, 12);
    expectPackets(serial.output, {0, 0x02});
    idle(remote, serial, 20);
    assert(remote.bytesSuppressed == 5);
    input(remote, serial, packet(0x40), 21);
    expectPackets(serial.output, {0, 0x02, 0, 0x40});
    assert(remote.commandButton == 0);
  }

  // Only supported commands can start; recent physical presses block them.
  {
    PanelBridge remote;
    FakeSerial serial;
    assert(remote.ready(0) && !remote.targetReady(0));
    idle(remote, serial, 0);
    assert(!remote.start(0x20, false, 0));
    assert(!remote.start(0x02, true, 0));
    assert(!remote.start(0xFF, false, 0));
    input(remote, serial, packet(0x40), 300);
    assert(!remote.start(0x02, false, 300));
  }

  // Sleeping-panel silence permits manual UP/DOWN and presets. Commands repeat
  // and release without panel input, but automatic mode still requires traffic.
  for (uint8_t button : {0x02, 0x04, 0x08, 0x20, 0x40}) {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    const bool hold = button == 0x20 || button == 0x40;
    const uint32_t duration = hold ? PanelBridge::holdLeaseMs : PanelBridge::presetMs;
    const uint32_t start = 24000;
    assert(remote.ready(start) && !remote.targetReady(start));
    assert(!remote.startTarget(0x20, start));
    assert(remote.start(button, hold, start));
    std::vector<uint8_t> expected;
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 10) {
      remote.forward(serial, start + elapsed);
      assert(remote.commandButton == button);
      expected.push_back(button);
    }
    remote.forward(serial, start + duration);
    expected.push_back(0);
    expectPackets(serial.output, expected);
    assert(remote.commandButton == 0);
    assert(!remote.ready(start + duration + 99));
    assert(remote.ready(start + duration + 100));
  }

  // A renewed silent-panel hold runs beyond the old 250 ms panel timeout and
  // releases explicitly; a waking physical panel still overrides it.
  {
    PanelBridge remote;
    FakeSerial serial;
    assert(remote.start(0x20, true, 0));
    remote.forward(serial, 0);
    expectPackets(serial.output, {0x20});
    for (uint32_t now = 100; now <= 1000; now += 100) {
      assert(remote.renew(remote.commandId, 0x20, now));
      remote.forward(serial, now);
      assert(remote.commandButton == 0x20);
    }
    input(remote, serial, packet(0x04), 1010);
    assert(remote.commandButton == 0 && remote.physicalOverrides == 1);
    assert(!remote.renew(remote.commandId, 0x20, 1020));
    assert(serial.output[serial.output.size() - 3] == 0x04);
  }

  // New malformed traffic cancels a hold even with no prior panel heartbeat.
  {
    PanelBridge remote;
    FakeSerial serial;
    assert(remote.start(0x40, true, 0));
    remote.forward(serial, 0);
    input(remote, serial, {0xA5, 0, 0, 0, 1}, 10);
    assert(remote.commandButton == 0 && remote.invalidFrames == 1);
    assert(!remote.ready(20));
    remote.forward(serial, 40);
    assert(remote.ready(260));
    assert(!remote.targetReady(260));
  }

  // Presets repeat at 10 ms, suppress panel idle, then send release at 100 ms.
  // Repeating the same preset after a release gap is a distinct press.
  for (uint8_t button : {0x02, 0x04, 0x08}) {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(button, false, 0));
    assert(!remote.start(button, false, 0));
    remote.forward(serial, 0);
    remote.forward(serial, 9);
    expectPackets(serial.output, {button});
    for (uint32_t now = 10; now < 100; now += 10) idle(remote, serial, now);
    assert(remote.bytesSuppressed == 45 && remote.espBytesSent == 50);
    remote.forward(serial, 100);
    assert(remote.commandButton == 0 && remote.espBytesSent == 55);
    std::vector<uint8_t> expected(10, button);
    expected.push_back(0);
    expectPackets(serial.output, expected);
    assert(!remote.start(button, false, 199));
    assert(remote.start(button, false, 200));
    remote.forward(serial, 200);
    expected.push_back(button);
    expectPackets(serial.output, expected);
  }

  // Holds require matching renewals; loss of renewals releases after 300 ms.
  for (uint8_t button : {0x20, 0x40}) {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(button, true, 0));
    const uint32_t id = remote.commandId;
    remote.forward(serial, 0);
    assert(!remote.renew(id + 1, button, 100));
    assert(!remote.renew(id, button == 0x20 ? 0x40 : 0x20, 100));
    idle(remote, serial, 100);
    assert(remote.renew(id, button, 100));
    idle(remote, serial, 200);
    idle(remote, serial, 399);
    assert(remote.commandButton == button);
    idle(remote, serial, 400);
    assert(remote.commandButton == 0);
    assert(!remote.renew(id, button, 400));
    assert(serial.output[serial.output.size() - 3] == 0);
  }

  // An explicit release is id-scoped; a delayed old release cannot stop a new hold.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    assert(remote.start(0x20, true, 0));
    const uint32_t oldId = remote.commandId;
    remote.forward(serial, 0);
    assert(!remote.release(0, 1));
    assert(remote.release(oldId, 1));
    remote.forward(serial, 10);
    assert(!remote.start(0x40, true, 100));
    assert(!remote.start(0x40, true, 109));
    assert(remote.start(0x40, true, 110));
    assert(!remote.release(oldId, 111));
    assert(remote.commandButton == 0x40);
  }

  // Every physical button, including unknown/programming commands, overrides
  // web control. Stale browser renewals cannot restart the cancelled hold.
  for (uint8_t physical : {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}) {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(0x20, true, 0));
    const uint32_t id = remote.commandId;
    remote.forward(serial, 0);
    input(remote, serial, packet(physical), 10);
    assert(remote.commandButton == 0 && remote.physicalOverrides == 1);
    expectPackets(serial.output, {0x20, 0, physical});
    assert(!remote.renew(id, 0x20, 11));
    assert(!remote.start(0x20, true, 111));
    idle(remote, serial, 120);
    assert(!remote.renew(id, 0x20, 120));
    assert(remote.start(0x20, true, 120));
  }

  // A physical press of the same preset still gets a release edge.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(0x02, false, 0));
    remote.forward(serial, 0);
    input(remote, serial, packet(0x02), 10);
    expectPackets(serial.output, {0x02, 0, 0x02});
  }

  // No synthetic packet is inserted into a split panel frame.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(0x02, false, 0));
    input(remote, serial, {0xA5, 0}, 0);
    assert(serial.output.empty());
    input(remote, serial, {0x40, 0, 0x40}, 1);
    expectPackets(serial.output, {0x40});
    assert(remote.commandButton == 0);
  }

  // Corrupt frames cancel web control but still pass through byte for byte.
  // Embedded headers recover without duplicating or losing raw bytes.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    serial.output.clear();
    assert(remote.start(0x20, true, 0));
    input(remote, serial, {0xA5, 0x99, 0xA5, 0, 0x40, 0, 0x40}, 1);
    assert(remote.commandButton == 0 && remote.invalidFrames == 1);
    assert((serial.output == std::vector<uint8_t>{0xA5, 0x99, 0xA5, 0, 0x40, 0, 0x40}));
    input(remote, serial, {0xA5, 0}, 2);
    remote.forward(serial, 32);
    assert(remote.partialTimeouts == 1);
    assert(remote.bytesReceived == remote.bytesForwarded);
  }

  // UART overflow, expired leases, and write failure cancel commands.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    assert(remote.start(0x20, true, 0));
    remote.forward(serial, 0);
    remote.inputOverflow(1);
    assert(remote.commandButton == 0 && !remote.ready(101));
    remote.forward(serial, 10);
    idle(remote, serial, 110);
    assert(remote.start(0x20, true, 110));
    remote.forward(serial, 110);
    remote.forward(serial, 360);
    assert(remote.commandButton == 0x20);
    remote.forward(serial, 410);
    assert(remote.commandButton == 0);
    idle(remote, serial, 520);
    assert(remote.start(0x20, true, 520));
    serial.failWrite = true;
    remote.forward(serial, 520);
    assert(remote.commandButton == 0 && remote.writeFailures == 5);
  }

  // Renewal cannot extend a single hold past its absolute ten-second limit.
  {
    PanelBridge remote;
    FakeSerial serial;
    idle(remote, serial, 0);
    assert(remote.start(0x20, true, 0));
    const uint32_t id = remote.commandId;
    for (uint32_t now = 0; now < PanelBridge::maxHoldMs; now += 100) {
      idle(remote, serial, now);
      assert(remote.renew(id, 0x20, now));
    }
    idle(remote, serial, PanelBridge::maxHoldMs);
    assert(remote.commandButton == 0);
    assert(!remote.renew(id, 0x20, PanelBridge::maxHoldMs));
  }

  // Preset timeout and cooldown work across millis wraparound.
  {
    PanelBridge remote;
    FakeSerial serial;
    const uint32_t start = UINT32_MAX - 50;
    idle(remote, serial, start);
    assert(remote.start(0x08, false, start));
    remote.forward(serial, start);
    idle(remote, serial, 48);
    assert(remote.commandButton == 0x08);
    remote.forward(serial, 49);
    assert(remote.commandButton == 0);
    remote.forward(serial, 59);
    assert(!remote.start(0x08, false, 158));
    assert(remote.start(0x08, false, 159));
  }
  puts("All panel bridge tests passed.");
}

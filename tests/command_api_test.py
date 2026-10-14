"""Run the firmware's HTTP handlers and serial servicing against host doubles."""

from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.cpp").read_text()
service = source[source.index("void serviceSerial() {"):source.index("const char *remoteButtonName()")]
handlers = source[source.index("void appendTargetStatus("):source.index("String heightString()")]
for route, handler in [("target", "setTarget"), ("target/cancel", "cancelTarget")]:
    assert f'server.on("/api/{route}", HTTP_POST, {handler});' in source

stub = r'''
#include "target_move.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define F(text) text
class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator+=;
  explicit String(uint32_t number) : std::string(std::to_string(number)) {}
  String &operator+=(uint32_t number) { append(std::to_string(number)); return *this; }
};
struct FakeServer {
  std::map<std::string, String> args;
  int code = 0;
  String body;
  String arg(const char *name) { return args[name]; }
  bool hasArg(const char *name) { return args.count(name); }
  void sendHeader(const char *, const char *) {}
  void send(int status, const char *, const String &json) { code = status; body = json; }
} server;
struct FakeSerial {
  std::vector<uint8_t> input, output;
  std::vector<uint8_t> arriveAfterEmpty;
  unsigned position = 0;
  bool overrun = false;
  int available() {
    const int count = input.size() - position;
    if (!count && !arriveAfterEmpty.empty()) {
      // Model bytes arriving just after this call observes an empty buffer.
      input.insert(input.end(), arriveAfterEmpty.begin(), arriveAfterEmpty.end());
      arriveAfterEmpty.clear();
    }
    return count;
  }
  int read() { return input[position++]; }
  unsigned write(uint8_t byte) { output.push_back(byte); return 1; }
  bool overflow() { bool result = overrun; overrun = false; return result; }
} panelSerial, statusSerial;
DeskMonitor desk;
PanelBridge panel;
TargetMove target;
uint32_t now = 0, uartOverruns = 0, panelUartOverruns = 0;
uint32_t millis() { return now; }
void height(uint16_t value) {
  const uint8_t codes[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const bool decimal = value < 10000;
  value /= decimal ? 10 : 100;
  uint8_t frame[] = {0x5A, codes[value / 100], codes[value / 10 % 10], codes[value % 10], 0x10, 0};
  if (decimal) frame[2] |= 0x80;
  frame[5] = frame[1] + frame[2] + frame[3] + frame[4];
  statusSerial.input.insert(statusSerial.input.end(), frame, frame + 6);
}
void idle() { panelSerial.input.insert(panelSerial.input.end(), {0xA5, 0, 0, 0, 0}); }
void result(int expected) {
  if (server.code != expected)
    std::cerr << "At " << now << " ms expected " << expected << ", got " << server.code
              << ": " << server.body << '\n';
  assert(server.code == expected);
  std::cout << server.body << '\n';
}
'''

checks = r'''
int main() {
  uint16_t parsedHeight;
  for (const char *bad : {"", "67", "121", "-68", "nan", "inf", "95.5", "95x", "95.00", "95e0", "99999999999"}) {
    server.args = {{"height_cm", bad}};
    assert(!readTargetHeight(parsedHeight));
    setTarget(); result(400);
    assert(panel.commandId == 0 && !target.active());
  }
  for (const char *valid : {"68", "95", "120", "95.0", "120.0"}) {
    server.args = {{"height_cm", valid}};
    assert(readTargetHeight(parsedHeight));
  }
  uint32_t parsedId;
  for (const char *bad : {"", "0", "-1", "1.0", "1x", "4294967296"}) {
    server.args = {{"id", bad}};
    assert(!readCommandId(parsedId));
    cancelTarget(); result(400);
  }
  server.args = {{"id", "4294967295"}};
  assert(readCommandId(parsedId) && parsedId == UINT32_MAX);

  server.args = {{"height_cm", "95"}};
  setTarget(); result(409);
  assert(panelSerial.output.empty());
  height(7000); idle(); serviceSerial();
  setTarget(); result(200);
  const uint32_t firstId = target.commandId;
  assert(target.active() && panel.commandButton == 0x20);
  serviceSerial();
  server.args = {{"direction", "up"}, {"id", std::to_string(firstId).c_str()}};
  moveDesk(); result(409); // Browser holds cannot extend an automatic command.
  assert(target.active());
  server.args = {{"height_cm", "80"}};
  setTarget(); result(409);
  assert(target.targetHundredths == 9500);
  now = 10;
  statusSerial.input.insert(statusSerial.input.end(), {0x5A, 0, 0, 0, 0x10, 0x10});
  height(7010); idle();
  panelSerial.output.clear();
  serviceSerial();
  assert(!target.active() && std::string(target.reason) == "invalid_display");
  assert(panel.commandButton == 0 && desk.automaticHeightFresh(now));
  for (unsigned i = 0; i < panelSerial.output.size(); i += 5)
    assert(panelSerial.output[i + 2] == 0); // No command follows the bad display.

  now = 110; height(7010); idle(); serviceSerial();
  server.args = {{"height_cm", "95"}};
  setTarget(); result(200); serviceSerial();
  const uint32_t secondId = target.commandId;
  server.args = {{"id", std::to_string(firstId).c_str()}};
  cancelTarget(); result(409);
  assert(target.active());
  server.args = {{"id", std::to_string(secondId).c_str()}};
  cancelTarget(); result(200);
  assert(!target.active() && std::string(target.reason) == "cancelled");
  serviceSerial();
  now = 120; height(7010); idle(); serviceSerial();

  now = 220; height(7010); idle(); serviceSerial();
  server.args = {{"height_cm", "95"}};
  setTarget(); result(200); serviceSerial();
  now = 230; height(9500); idle();
  panelSerial.output.clear();
  serviceSerial();
  assert(target.state == TargetMove::State::Complete && panel.commandButton == 0);
  for (unsigned i = 0; i < panelSerial.output.size(); i += 5)
    assert(panelSerial.output[i + 2] == 0); // Stop before the next outgoing press.
  now = 330; height(9500); idle(); serviceSerial();
  panelSerial.output.clear();
  server.args = {{"height_cm", "95"}};
  setTarget(); result(200);
  assert(!target.active() && panelSerial.output.empty());

  server.args = {{"height_cm", "120"}};
  setTarget(); result(200); serviceSerial();
  now = 340;
  height(9510);
  panelSerial.input.insert(panelSerial.input.end(), {0xA5, 0, 0x04, 0, 0x04});
  serviceSerial();
  assert(!target.active() && std::string(target.reason) == "panel_override");
  assert(panelSerial.output[panelSerial.output.size() - 3] == 0x04);
  now = 440; height(9510); idle(); serviceSerial();
  server.args = {{"height_cm", "120"}};
  setTarget(); result(200); serviceSerial();
  now = 450;
  statusSerial.overrun = true;
  height(9520); idle(); serviceSerial();
  assert(!target.active() && std::string(target.reason) == "status_uart_overflow");
  assert(uartOverruns == 1);
  now = 550; height(9520); idle(); serviceSerial();
  server.args = {{"height_cm", "120"}};
  setTarget(); result(200); serviceSerial();
  now = 800; idle(); serviceSerial(); // Only panel traffic, no new height.
  assert(!target.active() && std::string(target.reason) == "height_stale");

  const auto prepare = []() {
    desk = DeskMonitor();
    panel = PanelBridge();
    target = TargetMove();
    panelSerial = FakeSerial();
    statusSerial = FakeSerial();
    now = 0;
    height(7000); idle(); serviceSerial();
    server.args = {{"height_cm", "80"}};
    setTarget(); result(200); serviceSerial();
    assert(target.active());
  };
  // A new frame arriving after an empty-buffer observation is ordinary traffic,
  // not a backlog, even if this pass already processed many bytes.
  for (unsigned received : {0, 6, 252}) {
    prepare();
    height(7010);
    statusSerial.arriveAfterEmpty.assign(statusSerial.input.begin() + statusSerial.position,
                                         statusSerial.input.end());
    statusSerial.input.resize(statusSerial.position);
    for (unsigned i = 0; i < received / 6; ++i) height(7000);
    serviceSerial();
    assert(target.active() && panel.commandButton == 0x20);
    assert(statusSerial.available() == 6);
    serviceSerial();
    assert(target.active() && desk.heightHundredths == 7010);
  }
  // A genuinely exhausted processing budget still cancels before another press.
  prepare();
  for (unsigned i = 0; i < 43; ++i) height(7000);
  panelSerial.output.clear();
  serviceSerial();
  assert(statusSerial.available() == 2);
  assert(!target.active() && std::string(target.reason) == "status_backlog");
  assert(panel.commandButton == 0);
}
'''

with tempfile.TemporaryDirectory(prefix="api-test-", dir=root / ".pio") as directory:
    binary = str(Path(directory) / "command_api_test")
    subprocess.run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-fsanitize=address,undefined", "-I", str(root / "include"),
                    "-", "-o", binary], input=stub + service + handlers + checks,
                   text=True, check=True)
    completed = subprocess.run([binary], capture_output=True, text=True)
    if completed.returncode:
        print(completed.stderr)
        completed.check_returncode()
    for line in completed.stdout.splitlines():
        body = json.loads(line)
        assert isinstance(body["command_id"], int)
        assert isinstance(body["automatic_height_valid"], bool)
        assert isinstance(body["target_active"], bool)
        assert body["target_state"] in ["idle", "moving", "complete", "cancelled"]
print("All command API and serial integration tests passed.")

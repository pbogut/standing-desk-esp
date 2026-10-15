#include "mqtt_commands.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

static void message(MqttInbox &inbox, MqttCommandKind kind, const char *payload,
                    uint32_t now = 0, uint32_t sequence = 0, bool retain = false, bool dup = false) {
  const size_t size = strlen(payload);
  inbox.receive(kind, retain, dup, payload, size, 0, size, now, sequence);
}

int main() {
  MqttInbox inbox;
  MqttCommand command;
  assert(mqttCommandKind("home/desk/command/up", "home/desk") == MqttCommandKind::Invalid);
  assert(mqttCommandKind("home/desk/command/move", "home/desk") == MqttCommandKind::Move);
  assert(mqttCommandKind("home", "home/desk") == MqttCommandKind::Invalid);
  assert(mqttCommandKind("homeassistant/status", "home/desk") == MqttCommandKind::Birth);
  message(inbox, MqttCommandKind::Move, "up", 0, 5);
  assert(inbox.take(command, 249, 5) && command.value == 0x20);
  message(inbox, MqttCommandKind::Move, "down", 0, 5);
  assert(!inbox.take(command, 250, 5));
  message(inbox, MqttCommandKind::Move, "up", 0, 5);
  assert(!inbox.take(command, 1, 6));
  for (bool retained : {false, true}) {
    message(inbox, MqttCommandKind::Preset, "1", 0, 0, retained, !retained);
    assert(!inbox.take(command, 0, 0));
  }
  for (const char *value : {"67", "121", "nan", "95.5", "1e2", "95x", "", "-70"}) {
    message(inbox, MqttCommandKind::Target, value);
    assert(!inbox.take(command, 0, 0));
  }
  for (const char *value : {"68", "95.0", "120"}) {
    message(inbox, MqttCommandKind::Target, value);
    assert(inbox.take(command, 0, 0));
    assert(command.value >= 68 && command.value <= 120);
  }
  inbox.receive(MqttCommandKind::Move, false, false, "do", 2, 0, 4, 0, 5);
  inbox.receive(MqttCommandKind::Move, false, false, "wn", 2, 2, 4, 10, 5);
  assert(inbox.take(command, 10, 5) && command.value == 0x40);
  inbox.receive(MqttCommandKind::Move, false, false, "do", 2, 0, 4, 0, 5);
  inbox.receive(MqttCommandKind::Move, false, false, "wn", 2, 2, 4, 500, 5);
  assert(!inbox.take(command, 500, 5));
  inbox.receive(MqttCommandKind::Move, false, false, "do", 2, 0, 4, 0, 5);
  inbox.receive(MqttCommandKind::Move, false, false, "wn", 2, 1, 4, 10, 5);
  assert(!inbox.take(command, 10, 5));
  message(inbox, MqttCommandKind::Preset, "2");
  message(inbox, MqttCommandKind::Preset, "3");
  assert(inbox.take(command, 0, 0) && command.value == 0x04);
  message(inbox, MqttCommandKind::Target, "95");
  message(inbox, MqttCommandKind::Cancel, "cancel");
  assert(inbox.take(command, 0, 0) && command.kind == MqttCommandKind::Cancel);
  message(inbox, MqttCommandKind::Birth, "online", 0, 0, true);
  assert(inbox.birthRequested && !inbox.take(command, 0, 0));
  message(inbox, MqttCommandKind::Move, "up");
  inbox.clear();
  assert(!inbox.take(command, 0, 0) && !inbox.birthRequested);
  message(inbox, MqttCommandKind::Move, "up", UINT32_MAX - 100);
  assert(inbox.take(command, 100, 0));
  puts("All MQTT command tests passed.");
}

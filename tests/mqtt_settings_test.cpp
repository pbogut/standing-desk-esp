#include "mqtt_settings.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

int main() {
  MqttSettings settings, loaded;
  assert(!settings.valid());
  strcpy(settings.prefix, "standing-desk/abc123");
  assert(settings.valid());
  settings.enabled = true;
  assert(!settings.valid());
  strcpy(settings.host, "192.168.1.50");
  assert(settings.valid());
  strcpy(settings.username, "desk");
  strcpy(settings.password, "test-password");
  uint8_t record[MqttSettings::recordSize];
  settings.encode(record);
  assert(loaded.decode(record) && loaded.enabled && loaded.port == 1883);
  assert(!strcmp(loaded.password, settings.password));
  assert(!strcmp(loaded.prefix, settings.prefix));
  for (unsigned i = 0; i < sizeof(record); ++i) {
    record[i] ^= 1;
    assert(!loaded.decode(record));
    record[i] ^= 1;
  }
  for (const char *prefix : {"home/office/desk", "desk-1/test_2.v3"})
    assert(MqttSettings::topicPrefixValid(prefix));
  for (const char *prefix : {"", "/desk", "desk/", "desk//office", "desk/+", "desk/#", "desk name", "desk\""})
    assert(!MqttSettings::topicPrefixValid(prefix));
  settings.port = 0;
  assert(!settings.valid());
  settings.port = 65535;
  settings.username[0] = 0;
  assert(!settings.valid());
  settings.password[0] = 0;
  assert(settings.valid());
  strcpy(settings.host, "mqtt://oldbox");
  assert(!settings.valid());
  strcpy(settings.host, "oldbox.lan");
  assert(settings.valid());
  puts("All MQTT settings tests passed.");
}

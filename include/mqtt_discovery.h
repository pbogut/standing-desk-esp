#pragma once

#include "mqtt_settings.h"
#include <stdio.h>

struct MqttEntity {
  const char *component, *key, *name, *command, *payload, *extra;
  bool state;
};

static const MqttEntity mqttEntities[] = {
  {"sensor", "height", "Height", "", "", ",\"unit_of_measurement\":\"cm\",\"device_class\":\"distance\",\"state_class\":\"measurement\",\"value_template\":\"{{ value_json.height_cm }}\"", true},
  {"binary_sensor", "height_fresh", "Height fresh", "", "", ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"value_template\":\"{{ 'ON' if value_json.height_fresh else 'OFF' }}\"", true},
  {"button", "preset_1", "Preset 1", "preset", "1", "", false},
  {"button", "preset_2", "Preset 2", "preset", "2", "", false},
  {"button", "preset_3", "Preset 3", "preset", "3", "", false},
  {"button", "up", "Up", "move", "up", "", false},
  {"button", "down", "Down", "move", "down", "", false},
  {"number", "target", "Target height", "target", "", ",\"min\":68,\"max\":120,\"step\":1,\"mode\":\"box\",\"unit_of_measurement\":\"cm\",\"value_template\":\"{{ value_json.target_height_cm }}\"", true},
  {"button", "cancel", "Cancel automatic move", "cancel", "cancel", "", false},
  {"sensor", "movement", "Automatic move status", "", "", ",\"value_template\":\"{{ value_json.target_state }}\"", true}
};
static constexpr unsigned mqttEntityCount = sizeof(mqttEntities) / sizeof(mqttEntities[0]);

inline bool mqttIdentifierValid(const char *text) {
  if (!text[0] || strlen(text) > 40) return false;
  for (const char *c = text; *c; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
          || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return false;
  return true;
}

inline bool mqttDiscoveryTopic(const char *id, unsigned index, char *out, size_t capacity) {
  if (index >= mqttEntityCount || !mqttIdentifierValid(id)) return false;
  const int length = snprintf(out, capacity, "homeassistant/%s/%s/%s/config",
                              mqttEntities[index].component, id, mqttEntities[index].key);
  return length >= 0 && static_cast<size_t>(length) < capacity;
}

inline bool mqttDiscoveryPayload(const char *id, const char *prefix, unsigned index,
                                char *out, size_t capacity) {
  if (index >= mqttEntityCount || !mqttIdentifierValid(id) || !MqttSettings::topicPrefixValid(prefix)) return false;
  const MqttEntity &entity = mqttEntities[index];
  char command[220] = {}, state[180] = {}, payload[64] = {}, attributes[180] = {};
  if (entity.command[0])
    snprintf(command, sizeof(command), ",\"command_topic\":\"%s/command/%s\",\"retain\":false", prefix, entity.command);
  if (entity.payload[0]) snprintf(payload, sizeof(payload), ",\"payload_press\":\"%s\"", entity.payload);
  if (entity.state) snprintf(state, sizeof(state), ",\"state_topic\":\"%s/state\"", prefix);
  if (!strcmp(entity.key, "movement"))
    snprintf(attributes, sizeof(attributes), ",\"json_attributes_topic\":\"%s/state\"", prefix);
  const int length = snprintf(out, capacity,
      "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"device\":{\"identifiers\":[\"%s\"],"
      "\"name\":\"Standing desk\",\"manufacturer\":\"DIY\",\"model\":\"Wemos D1 mini\","
      "\"configuration_url\":\"http://%s.local/\"},\"availability_topic\":\"%s/availability\","
      "\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"qos\":0%s%s%s%s%s}",
      entity.name, id, entity.key, id, id, prefix, command, state, payload, attributes, entity.extra);
  return length >= 0 && static_cast<size_t>(length) < capacity;
}

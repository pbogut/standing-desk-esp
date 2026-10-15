#pragma once

#include <stdint.h>
#include <string.h>

struct MqttSettings {
  bool enabled = false;
  uint16_t port = 1883;
  char host[65] = {}, username[65] = {}, password[129] = {}, prefix[97] = {};
  static constexpr unsigned recordSize = 367;

  static bool topicPrefixValid(const char *text) {
    const unsigned length = strlen(text);
    if (!length || length > 96 || text[0] == '/' || text[length - 1] == '/') return false;
    for (unsigned i = 0; i < length; ++i) {
      const char c = text[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/')) return false;
      if (c == '/' && i && text[i - 1] == '/') return false;
    }
    return true;
  }

  bool valid() const {
    if (!memchr(host, 0, sizeof(host)) || !memchr(username, 0, sizeof(username))
        || !memchr(password, 0, sizeof(password)) || !memchr(prefix, 0, sizeof(prefix))) return false;
    if (!port || (enabled && !host[0]) || (!username[0] && password[0]) || !topicPrefixValid(prefix)) return false;
    for (const char *c = host; *c; ++c)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
            || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_')) return false;
    return true;
  }

  void encode(uint8_t (&record)[recordSize]) const {
    memcpy(record, "DMQ1", 4);
    record[4] = enabled;
    record[5] = port & 0xFF;
    record[6] = port >> 8;
    memcpy(record + 7, host, sizeof(host));
    memcpy(record + 72, username, sizeof(username));
    memcpy(record + 137, password, sizeof(password));
    memcpy(record + 266, prefix, sizeof(prefix));
    const uint32_t sum = checksum(record);
    for (unsigned i = 0; i < 4; ++i) record[363 + i] = sum >> (8 * i);
  }

  bool decode(const uint8_t (&record)[recordSize]) {
    *this = MqttSettings();
    uint32_t stored = 0;
    for (unsigned i = 0; i < 4; ++i) stored |= uint32_t(record[363 + i]) << (8 * i);
    if (memcmp(record, "DMQ1", 4) || record[4] > 1 || checksum(record) != stored) return false;
    MqttSettings loaded;
    loaded.enabled = record[4];
    loaded.port = record[5] | uint16_t(record[6]) << 8;
    memcpy(loaded.host, record + 7, sizeof(host));
    memcpy(loaded.username, record + 72, sizeof(username));
    memcpy(loaded.password, record + 137, sizeof(password));
    memcpy(loaded.prefix, record + 266, sizeof(prefix));
    if (!loaded.valid()) return false;
    *this = loaded;
    return true;
  }

 private:
  static uint32_t checksum(const uint8_t *record) {
    uint32_t hash = 2166136261UL;
    for (unsigned i = 0; i < 363; ++i) hash = (hash ^ record[i]) * 16777619UL;
    return hash;
  }
};

class MqttSettingsStore {
 public:
  template <typename Filesystem>
  bool load(Filesystem &fs, MqttSettings &settings) const {
    settings = MqttSettings();
    auto file = fs.open("/mqtt.bin", "r");
    if (!file || file.size() != MqttSettings::recordSize) return false;
    uint8_t record[MqttSettings::recordSize];
    if (file.read(record, sizeof(record)) != sizeof(record)) return false;
    return settings.decode(record);
  }
  template <typename Filesystem>
  bool save(Filesystem &fs, const MqttSettings &settings) const {
    if (!settings.valid()) return false;
    uint8_t record[MqttSettings::recordSize];
    settings.encode(record);
    auto file = fs.open("/mqtt.tmp", "w");
    if (!file) return false;
    const bool written = file.write(record, sizeof(record)) == sizeof(record);
    file.flush();
    file.close();
    if (!written || !fs.rename("/mqtt.tmp", "/mqtt.bin")) {
      fs.remove("/mqtt.tmp");
      return false;
    }
    return true;
  }
};

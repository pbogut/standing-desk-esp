#pragma once

#include "wifi_settings.h"

class WifiCredentialStore {
 public:
  template <typename Filesystem>
  bool load(Filesystem &fs, WifiCredentials &credentials) const {
    credentials = WifiCredentials();
    auto file = fs.open("/wifi.bin", "r");
    if (!file || file.size() != WifiCredentials::recordSize) return false;
    uint8_t record[WifiCredentials::recordSize];
    if (file.read(record, sizeof(record)) != sizeof(record)) return false;
    return credentials.decode(record);
  }

  template <typename Filesystem>
  bool save(Filesystem &fs, const WifiCredentials &credentials) const {
    if (!credentials.valid()) return false;
    uint8_t record[WifiCredentials::recordSize];
    credentials.encode(record);
    auto file = fs.open("/wifi.tmp", "w");
    if (!file) return false;
    const bool written = file.write(record, sizeof(record)) == sizeof(record);
    file.flush();
    file.close();
    if (!written || !fs.rename("/wifi.tmp", "/wifi.bin")) {
      fs.remove("/wifi.tmp");
      return false;
    }
    return true;
  }

  template <typename Filesystem>
  bool forget(Filesystem &fs) const {
    if (fs.exists("/wifi.bin") && !fs.remove("/wifi.bin")) return false;
    fs.remove("/wifi.tmp");
    return true;
  }
};

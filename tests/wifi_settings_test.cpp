#include "wifi_settings.h"
#include "wifi_credential_store.h"

#include <assert.h>
#include <stdio.h>
#include <string>
#include <map>
#include <vector>

struct FakeFilesystem {
  std::map<std::string, std::vector<uint8_t>> files;
  bool failWrite = false, failRename = false, failRemove = false;
  struct File {
    std::vector<uint8_t> *data;
    bool failWrite;
    explicit operator bool() const { return data != nullptr; }
    unsigned size() const { return data->size(); }
    unsigned read(uint8_t *out, unsigned count) {
      if (count > data->size()) count = data->size();
      memcpy(out, data->data(), count);
      return count;
    }
    unsigned write(const uint8_t *in, unsigned count) {
      if (failWrite) return 0;
      data->assign(in, in + count);
      return count;
    }
    void flush() {}
    void close() { data = nullptr; }
  };
  File open(const char *path, const char *mode) {
    if (mode[0] == 'w') { files[path].clear(); return {&files[path], failWrite}; }
    return {files.count(path) ? &files[path] : nullptr, false};
  }
  bool exists(const char *path) const { return files.count(path); }
  bool remove(const char *path) {
    if (failRemove) return false;
    return files.erase(path);
  }
  bool rename(const char *from, const char *to) {
    if (failRename || !files.count(from)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

struct FakeNetwork {
  bool online = false, failAp = false, keptAp = false;
  unsigned connections = 0, apStarts = 0, apStops = 0, disconnections = 0;
  bool isConnected() const { return online; }
  void connect(bool ap) { ++connections; keptAp = ap; online = false; }
  void disconnect() { ++disconnections; online = false; }
  bool startAp(bool) { ++apStarts; return !failAp; }
  void stopAp() { ++apStops; }
};

int main() {
  {
    FakeFilesystem fs;
    WifiCredentialStore store;
    WifiCredentials original, replacement, restored;
    assert(!store.load(fs, restored) && !restored.valid());
    assert(original.set("first", "password-one"));
    assert(store.save(fs, original));
    assert(store.load(fs, restored) && !strcmp(restored.ssid, "first"));
    assert(!strcmp(restored.password, "password-one"));
    assert(replacement.set("second", "password-two"));
    fs.failWrite = true;
    assert(!store.save(fs, replacement));
    assert(store.load(fs, restored) && !strcmp(restored.ssid, "first"));
    fs.failWrite = false;
    fs.failRename = true;
    assert(!store.save(fs, replacement));
    assert(store.load(fs, restored) && !strcmp(restored.ssid, "first"));
    fs.failRename = false;
    assert(store.save(fs, replacement));
    assert(store.load(fs, restored) && !strcmp(restored.ssid, "second"));
    fs.failRemove = true;
    assert(!store.forget(fs));
    assert(store.load(fs, restored));
    fs.failRemove = false;
    assert(store.forget(fs));
    assert(!store.load(fs, restored));
    assert(store.save(fs, original));
    fs.files["/wifi.bin"].pop_back();
    assert(!store.load(fs, restored) && !restored.valid());
    assert(store.save(fs, original));
    fs.files["/wifi.bin"][20] ^= 1;
    assert(!store.load(fs, restored) && !restored.valid());
  }
  {
    WifiCredentials credentials, loaded;
    assert(!credentials.valid());
    assert(!credentials.set("", "password"));
    assert(!credentials.set(std::string(33, 's').c_str(), "password"));
    assert(!credentials.set("home", "short"));
    assert(!credentials.set("home", std::string(64, 'p').c_str()));
    assert(credentials.set(std::string(32, 's').c_str(), std::string(63, 'p').c_str()));
    uint8_t record[WifiCredentials::recordSize];
    credentials.encode(record);
    assert(loaded.decode(record));
    assert(!strcmp(loaded.ssid, credentials.ssid));
    assert(!strcmp(loaded.password, credentials.password));
    // Every byte is covered by version/length/terminator validation and checksum.
    for (unsigned i = 0; i < sizeof(record); ++i) {
      record[i] ^= 1;
      assert(!loaded.decode(record) && !loaded.valid());
      record[i] ^= 1;
    }
    assert(credentials.set("open network", ""));
    credentials.encode(record);
    assert(loaded.decode(record) && loaded.password[0] == 0);
    assert(!strcmp(loaded.ssid, "open network"));
  }
  // No credentials: immediate AP, no station connection attempts.
  {
    WifiConnection wifi;
    FakeNetwork network;
    wifi.configure(false, network, 0);
    assert(wifi.apActive && network.apStarts == 1 && !wifi.connected);
    wifi.update(network, 60000);
    assert(network.connections == 0 && network.apStarts == 1);
    // Saving from the AP keeps it running while the station connects.
    wifi.configure(true, network, 60000);
    assert(wifi.apActive && network.keptAp);
    network.online = true;
    wifi.update(network, 60001);
    wifi.update(network, 90000);
    assert(wifi.apActive);
    wifi.update(network, 90001);
    assert(!wifi.apActive && network.apStops == 1);
  }
  // Saved credentials: sixty seconds to connect, periodic retries and fallback.
  {
    WifiConnection wifi;
    FakeNetwork network;
    wifi.configure(true, network, 0);
    assert(!wifi.apActive && network.connections == 1 && !network.keptAp);
    assert(wifi.fallbackRemaining(1000) == 59000);
    wifi.update(network, 30000);
    assert(network.connections == 2 && network.apStarts == 0);
    wifi.update(network, 59999);
    assert(!wifi.apActive);
    wifi.update(network, 60000);
    assert(wifi.apActive && network.connections == 3 && network.keptAp);
    assert(wifi.fallbackRemaining(60000) == 0);
    wifi.update(network, 90000);
    assert(network.connections == 4 && wifi.apActive);
    network.online = true;
    wifi.update(network, 90001);
    wifi.update(network, 120001);
    assert(wifi.connected && !wifi.apActive);
    // An outage starts a new sixty-second window, not the boot-time window.
    network.online = false;
    wifi.update(network, 120002);
    assert(!wifi.connected && network.connections == 5);
    wifi.update(network, 180001);
    assert(!wifi.apActive);
    wifi.update(network, 180002);
    assert(wifi.apActive);
    wifi.configure(false, network, 180003);
    assert(!wifi.configured && wifi.apActive && network.disconnections == 1);
  }
  // A brief reconnection must not close the AP if station connectivity drops again.
  {
    WifiConnection wifi;
    FakeNetwork network;
    wifi.configure(false, network, 0);
    wifi.configure(true, network, 1);
    network.online = true;
    wifi.update(network, 2);
    network.online = false;
    wifi.update(network, 1000);
    assert(wifi.apActive && network.apStops == 0);
    network.online = true;
    wifi.update(network, 1001);
    wifi.update(network, 31000);
    assert(wifi.apActive);
    wifi.update(network, 31001);
    assert(!wifi.apActive);
  }
  // AP startup failure is retried without blocking or rapid busy-loop retries.
  {
    WifiConnection wifi;
    FakeNetwork network;
    network.failAp = true;
    wifi.configure(false, network, 0);
    wifi.update(network, 9999);
    assert(network.apStarts == 1 && !wifi.apActive);
    network.failAp = false;
    wifi.update(network, 10000);
    assert(network.apStarts == 2 && wifi.apActive);
  }
  // Fallback and grace periods work across millis wraparound.
  {
    WifiConnection wifi;
    FakeNetwork network;
    const uint32_t start = UINT32_MAX - 1000;
    wifi.configure(true, network, start);
    wifi.update(network, start + 59999);
    assert(!wifi.apActive);
    wifi.update(network, start + 60000);
    assert(wifi.apActive);
    network.online = true;
    wifi.update(network, start + 60001);
    wifi.update(network, start + 90001);
    assert(!wifi.apActive && wifi.connected);
  }
  puts("All Wi-Fi settings and connection tests passed.");
}

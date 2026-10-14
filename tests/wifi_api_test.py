"""Exercise the real Wi-Fi handlers and deferred network service with host doubles."""

from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.cpp").read_text()
wifi_code = source[source.index("struct DeskWifiNetwork {"):source.index("void setup() {")]
fs_test = (root / "tests/wifi_settings_test.cpp").read_text()
filesystem = fs_test[fs_test.index("struct FakeFilesystem {"):fs_test.index("struct FakeNetwork {")]
for method, route, handler in [("GET", "wifi", "sendWifiStatus"),
                                ("POST", "wifi", "saveWifiSettings"),
                                ("POST", "wifi/forget", "forgetWifiSettings")]:
    assert f'server.on("/api/{route}", HTTP_{method}, {handler});' in source

stub = r'''
#include "wifi_credential_store.h"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define F(text) text
class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator+=;
  String(const std::string &value) : std::string(value) {}
  String &operator+=(uint32_t number) { append(std::to_string(number)); return *this; }
};
struct FakeServer {
  std::map<std::string, String> args;
  int code = 0;
  String body;
  String arg(const char *name) { return args.count(name) ? args.at(name) : String(""); }
  bool hasArg(const char *name) { return args.count(name); }
  void sendHeader(const char *, const char *) {}
  void send(int status, const char *, const String &json) { code = status; body = json; }
} server;
struct IPAddress {
  unsigned a, b, c, d;
  IPAddress(unsigned a, unsigned b, unsigned c, unsigned d) : a(a), b(b), c(c), d(d) {}
  explicit operator uint32_t() const { return a | b << 8 | c << 16 | d << 24; }
  String toString() const {
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
  }
};
enum { WIFI_STA, WIFI_AP, WIFI_AP_STA, WL_CONNECTED };
struct FakeWifi {
  bool online = false, ap = false;
  unsigned connections = 0;
  String name, pass, hostName;
  int status() const { return online ? WL_CONNECTED : -1; }
  IPAddress localIP() const { return online ? IPAddress(192,168,1,50) : IPAddress(0,0,0,0); }
  IPAddress softAPIP() const { return IPAddress(192,168,4,1); }
  String SSID() const { return name; }
  void mode(int mode) { if(mode == WIFI_STA) ap=false; if(mode == WIFI_AP) online=false; }
  void disconnect(bool) { online=false; }
  void hostname(const String &value) { hostName=value; }
  void begin(const char *ssid, const char *password) { name=ssid; pass=password; ++connections; }
  void softAPConfig(IPAddress, IPAddress, IPAddress) {}
  bool softAP(const char *, const char *) { ap=true; return true; }
  void softAPdisconnect(bool) { ap=false; }
} WiFi;
struct FakeMdns {
  bool active=false;
  bool begin(const char *) { active=true; return true; }
  void end() { active=false; }
  void addService(const char *, const char *, int) {}
  void update() {}
} MDNS;
uint32_t now = 0, wifiChangeMs = 0;
uint32_t millis() { return now; }
WifiCredentials wifiCredentials;
WifiCredentialStore wifiStore;
WifiConnection wifiConnection;
String deviceName = "standing-desk-test";
bool wifiStorageReady = true, mdnsRunning = false;
enum class WifiChange { None, Connect, Forget };
WifiChange wifiChange = WifiChange::None;
const char apPassword[] = "test-ap-password";
'''

checks = r'''
int main() {
  wifiConnection.configure(false, wifiNetwork, now);
  assert(wifiConnection.apActive);
  const char *secret = "secret-test-password";
  const auto response = [secret](int expected) {
    assert(server.code == expected);
    assert(server.body.find(secret) == std::string::npos);
    std::cout << server.body << '\n';
  };
  server.args={{"ssid","home"}};
  saveWifiSettings(); response(400);
  server.args={{"ssid",std::string(33,'s')},{"password",secret}};
  saveWifiSettings(); response(400);
  server.args={{"ssid","home"},{"password","short"}};
  saveWifiSettings(); response(400);
  server.args={{"ssid",std::string("good\0evil",9)},{"password",secret}};
  saveWifiSettings(); response(400);
  assert(!wifiCredentials.valid() && LittleFS.files.empty() && WiFi.connections==0);

  const String name="home\"\\network";
  server.args={{"ssid",name},{"password",secret}};
  saveWifiSettings(); response(200);
  assert(wifiChange==WifiChange::Connect && WiFi.connections==0);
  WifiCredentials restored;
  assert(wifiStore.load(LittleFS,restored) && !strcmp(restored.password,secret));
  now=499; serviceWifi(); assert(WiFi.connections==0);
  now=500; serviceWifi(); assert(WiFi.connections==1 && WiFi.ap);
  WiFi.online=true;
  now=501; serviceWifi();
  assert(wifiConnection.connected && mdnsRunning && WiFi.ap);
  sendWifiStatus(); response(200);
  now=30501; serviceWifi(); assert(!WiFi.ap && wifiConnection.connected);
  WiFi.online=false;
  now=31000; serviceWifi(); assert(!wifiConnection.connected && !mdnsRunning);
  now=90999; serviceWifi(); assert(!WiFi.ap);
  now=91000; serviceWifi(); assert(WiFi.ap && wifiConnection.apActive);
  sendWifiStatus(); response(200);

  // Failed writes must preserve both saved settings and the current connection.
  server.args={{"ssid","replacement"},{"password","new-password"}};
  LittleFS.failWrite=true;
  saveWifiSettings(); response(500);
  assert(wifiChange==WifiChange::None && wifiCredentials.ssid==name);
  assert(wifiStore.load(LittleFS,restored) && restored.ssid==name);
  LittleFS.failWrite=false;
  LittleFS.failRemove=true;
  forgetWifiSettings(); response(500);
  assert(wifiCredentials.valid() && wifiChange==WifiChange::None);
  LittleFS.failRemove=false;
  forgetWifiSettings(); response(200);
  assert(!wifiCredentials.valid() && !wifiStore.load(LittleFS,restored));
  now=91500; serviceWifi();
  assert(!wifiConnection.configured && wifiConnection.apActive && WiFi.ap);
  sendWifiStatus(); response(200);

  // Open networks are explicit; the response never supplies a saved password.
  server.args={{"ssid","open network"},{"password",""}};
  saveWifiSettings(); response(200);
  assert(wifiCredentials.valid() && wifiCredentials.password[0]==0);
  assert(wifiStore.load(LittleFS,restored) && !strcmp(restored.ssid,"open network"));
  wifiStorageReady=false;
  saveWifiSettings(); response(500);
  forgetWifiSettings(); response(500);
}
'''

with tempfile.TemporaryDirectory(prefix="wifi-api-test-", dir=root / ".pio") as directory:
    binary = str(Path(directory) / "wifi_api_test")
    subprocess.run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-fsanitize=address,undefined", "-I", str(root / "include"),
                    "-", "-o", binary], input=stub + filesystem + "FakeFilesystem LittleFS;\n"
                    + wifi_code + checks, text=True, check=True)
    completed = subprocess.run([binary], capture_output=True, text=True)
    if completed.returncode:
        print(completed.stderr)
        completed.check_returncode()
    for line in completed.stdout.splitlines():
        body = json.loads(line)
        assert "password" not in body
        if "ssid" in body:
            assert isinstance(body["password_saved"], bool)
            assert isinstance(body["connected"], bool)
print("All Wi-Fi API, password handling, and deferred network tests passed.")

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
assert 'server.on("/api/wifi/scan", HTTP_POST, startWifiScan);' in source
assert 'server.on("/api/wifi/scan", HTTP_GET,' in source

stub = r'''
#include "wifi_credential_store.h"
#include "wifi_scan.h"
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
  explicit String(int32_t value) : std::string(std::to_string(value)) {}
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
enum { WIFI_STA, WIFI_AP, WIFI_AP_STA, WL_CONNECTED, ENC_TYPE_NONE, ENC_TYPE_CCMP,
       WIFI_SCAN_RUNNING=-1, WIFI_SCAN_FAILED=-2 };
struct FakeWifi {
  bool online = false, ap = false;
  unsigned connections = 0;
  String name, pass, hostName;
  struct ScanNetwork { String ssid; int32_t rssi; bool secured; };
  std::vector<ScanNetwork> scanResults;
  int scanResult=WIFI_SCAN_FAILED, scanStartResult=WIFI_SCAN_RUNNING;
  unsigned scanStarts=0, scanDeletes=0;
  bool autoReconnect=true;
  int status() const { return online ? WL_CONNECTED : -1; }
  IPAddress localIP() const { return online ? IPAddress(192,168,1,50) : IPAddress(0,0,0,0); }
  IPAddress softAPIP() const { return IPAddress(192,168,4,1); }
  String SSID() const { return name; }
  String SSID(int i) const { return scanResults[i].ssid; }
  int32_t RSSI(int i) const { return scanResults[i].rssi; }
  int encryptionType(int i) const { return scanResults[i].secured ? ENC_TYPE_CCMP : ENC_TYPE_NONE; }
  void setAutoReconnect(bool enabled) { autoReconnect=enabled; }
  int scanNetworks(bool async, bool showHidden) {
    assert(async && !showHidden);
    ++scanStarts; scanResult=scanStartResult; return scanResult;
  }
  int scanComplete() const { return scanResult; }
  void scanDelete() { ++scanDeletes; scanResults.clear(); scanResult=WIFI_SCAN_FAILED; }
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
WifiScan wifiScan;
bool wifiScanCleanupPending=false;
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

  wifiCredentials=WifiCredentials(); wifiConnection=WifiConnection();
  WiFi=FakeWifi(); wifiScan=WifiScan(); LittleFS=FakeFilesystem();
  wifiChange=WifiChange::None; wifiStorageReady=true; now=0;
  wifiConnection.configure(false,wifiNetwork,now);
  sendWifiScan(); response(200);
  startWifiScan(); response(202);
  assert(wifiScan.running() && WiFi.scanStarts==1 && !WiFi.autoReconnect);
  startWifiScan(); response(202);
  assert(WiFi.scanStarts==1); // Repeated requests reuse the in-flight scan.
  saveWifiSettings(); response(409);
  forgetWifiSettings(); response(409);
  assert(LittleFS.files.empty());
  now=1000; serviceWifi();
  assert(wifiScan.running() && WiFi.ap && WiFi.connections==0);
  WiFi.scanResults={{"weak",-90,true},{"quoted\"\\ssid",-40,true},
                    {"open",-50,false},{"open",-70,false},{"",-30,true}};
  WiFi.scanResult=WiFi.scanResults.size();
  serviceWifi();
  assert(!wifiScan.running() && wifiScan.count==3 && WiFi.scanDeletes==1);
  assert(WiFi.autoReconnect && !wifiCredentials.valid());
  assert(!strcmp(wifiScan.networks[0].ssid,"quoted\"\\ssid"));
  sendWifiScan(); response(200);

  startWifiScan(); response(202);
  WiFi.scanResult=0; serviceWifi();
  assert(wifiScan.state==WifiScan::State::Complete && wifiScan.count==0);
  sendWifiScan(); response(200);
  WiFi.scanStartResult=WIFI_SCAN_FAILED;
  startWifiScan(); response(500);
  assert(!wifiScan.running() && WiFi.autoReconnect);
  WiFi.scanStartResult=WIFI_SCAN_RUNNING;
  startWifiScan(); response(202);
  WiFi.scanResult=WIFI_SCAN_FAILED; serviceWifi();
  assert(wifiScan.state==WifiScan::State::Failed && WiFi.autoReconnect);
  sendWifiScan(); response(200);

  startWifiScan(); response(202);
  now+=WifiScan::timeoutMs; serviceWifi();
  assert(wifiScan.state==WifiScan::State::Failed && wifiScanCleanupPending);
  assert(WiFi.autoReconnect);
  sendWifiScan(); response(200);
  startWifiScan(); response(409); // SDK has not yet finished the timed-out scan.
  WiFi.scanResult=0; serviceWifi();
  assert(!wifiScanCleanupPending);
  wifiChange=WifiChange::Connect;
  startWifiScan(); response(409);
  wifiChange=WifiChange::None;

  // Don't interrupt initial STA-only connection attempts. On fallback AP, pause
  // retries for the scan and resume promptly when results arrive.
  wifiConnection=WifiConnection(); WiFi=FakeWifi(); now=0;
  assert(wifiCredentials.set("home",secret));
  wifiConnection.configure(true,wifiNetwork,now);
  startWifiScan(); response(409);
  assert(WiFi.scanStarts==0);
  now=60000; serviceWifi(); assert(wifiConnection.apActive);
  const unsigned connections=WiFi.connections;
  now=89000; startWifiScan(); response(202);
  now=90000; serviceWifi();
  assert(WiFi.connections==connections && WiFi.ap);
  WiFi.scanResult=0; now=90001; serviceWifi();
  assert(WiFi.connections==connections+1 && WiFi.autoReconnect);
  WiFi.online=true; now=90002; serviceWifi();
  now=119000; startWifiScan(); response(202);
  now=120002; serviceWifi(); assert(WiFi.ap);
  WiFi.scanResult=0; now=120003; serviceWifi();
  assert(WiFi.ap && wifiConnection.connected);
  now=150002; serviceWifi(); assert(WiFi.ap);
  now=150003; serviceWifi(); assert(!WiFi.ap);
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
        if "networks" in body:
            assert body["state"] in ["idle", "running", "complete", "failed"]
            networks = body["networks"]
            assert len(networks) <= 20
            assert all(isinstance(network["secured"], bool) for network in networks)
            assert [network["rssi"] for network in networks] == sorted(
                [network["rssi"] for network in networks], reverse=True)
            assert all(network["rssi"] < 0 for network in networks)
print("All Wi-Fi API, password handling, and deferred network tests passed.")

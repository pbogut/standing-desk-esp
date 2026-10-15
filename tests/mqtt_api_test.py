from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.cpp").read_text()
handlers = source[source.index("String mqttStatusJson() {"):source.index("struct DeskWifiNetwork {")]
quoted = source[source.index("String jsonQuoted(const String &text) {"):source.index("String wifiStatusJson() {")]
fs_source = (root / "tests/wifi_settings_test.cpp").read_text()
filesystem = fs_source[fs_source.index("struct FakeFilesystem {"):fs_source.index("struct FakeNetwork {")]
assert 'server.on("/api/mqtt", HTTP_GET, sendMqttStatus);' in source
assert 'server.on("/api/mqtt", HTTP_POST, saveMqttSettings);' in source

stub = r'''
#include "mqtt_settings.h"
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
  String &operator+=(uint32_t value) { append(std::to_string(value)); return *this; }
  String &operator+=(uint16_t value) { append(std::to_string(value)); return *this; }
};
struct FakeServer {
  std::map<std::string,String> args;
  int code=0;
  String body;
  String arg(const char *key) { return args.count(key)?args.at(key):String(""); }
  bool hasArg(const char *key) { return args.count(key); }
  void sendHeader(const char *,const char *) {}
  void send(int status,const char *,const String &json) { code=status;body=json; }
} server;
struct FakeMqtt {
  unsigned configured=0;
  void configure(const MqttSettings &) { ++configured; }
  bool connected() const { return false; }
  const char *stateName() const { return "disabled"; }
  const char *error() const { return ""; }
  const char *commandError() const { return ""; }
  uint32_t accepted() const { return 0; }
  uint32_t rejected() const { return 0; }
} mqtt;
MqttSettings mqttSettings;
MqttSettingsStore mqttStore;
bool wifiStorageReady=true;
'''

checks = r'''
int main() {
  strcpy(mqttSettings.prefix,"standing-desk/abc123");
  const auto response=[](int code) {
    assert(server.code==code);
    assert(server.body.find("mqtt-secret")==std::string::npos);
    std::cout<<server.body<<'\n';
  };
  sendMqttStatus();response(200);
  const std::map<std::string,String> good={{"enabled","true"},{"host","192.168.1.50"},
    {"port","1883"},{"username","desk"},{"password","mqtt-secret"},{"topic_prefix","home/office/desk"}};
  server.args=good;saveMqttSettings();response(200);
  assert(mqtt.configured==1 && mqttSettings.enabled);
  MqttSettings restored;
  assert(mqttStore.load(LittleFS,restored) && !strcmp(restored.password,"mqtt-secret"));
  for(const char *port:{"0","65536","-1","1883x","nan"}) {
    server.args=good;server.args["port"]=port;
    saveMqttSettings();response(400);
  }
  for(const char *prefix:{"desk/+","desk/#","/desk","desk/","a//b","a b"}) {
    server.args=good;server.args["topic_prefix"]=prefix;
    saveMqttSettings();response(400);
  }
  server.args=good;server.args["host"]="mqtt://oldbox";saveMqttSettings();response(400);
  server.args=good;server.args.erase("password");saveMqttSettings();response(400);
  server.args=good;server.args["username"]=std::string("ab\0cd",5);saveMqttSettings();response(400);
  assert(mqtt.configured==1);
  server.args=good;server.args["password"]="";server.args["enabled"]="false";
  saveMqttSettings();response(200);
  assert(!mqttSettings.enabled && !strcmp(mqttSettings.password,"mqtt-secret"));
  server.args["clear_password"]="true";saveMqttSettings();response(200);
  assert(!mqttSettings.password[0]);
  server.args=good;LittleFS.failWrite=true;saveMqttSettings();response(500);
  assert(!mqttSettings.enabled && mqttStore.load(LittleFS,restored) && !restored.password[0]);
  LittleFS.failWrite=false;LittleFS.failRename=true;saveMqttSettings();response(500);
  assert(mqttStore.load(LittleFS,restored) && !restored.password[0]);
  LittleFS.failRename=false;wifiStorageReady=false;saveMqttSettings();response(500);
}
'''
with tempfile.TemporaryDirectory(prefix="mqtt-api-test-", dir=root / ".pio") as directory:
    binary = str(Path(directory) / "mqtt_api_test")
    subprocess.run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-fsanitize=address,undefined", "-I", str(root / "include"),
                    "-", "-o", binary], input=stub+filesystem+"FakeFilesystem LittleFS;\n"+quoted+handlers+checks,
                   text=True, check=True)
    completed = subprocess.run([binary], capture_output=True, text=True)
    if completed.returncode:
        print(completed.stderr)
        completed.check_returncode()
    for line in completed.stdout.splitlines():
        result = json.loads(line)
        assert "password" not in result
        if "host" in result:
            assert isinstance(result["password_saved"], bool)
            assert isinstance(result["port"], int)
print("All MQTT settings API and persistence tests passed.")

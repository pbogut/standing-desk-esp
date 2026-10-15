from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
code = r'''
#include "mqtt_discovery.h"
#include <assert.h>
#include <stdio.h>
int main() {
  char topic[160], config[1280];
  for (unsigned i = 0; i < mqttEntityCount; ++i) {
    assert(mqttDiscoveryTopic("standing-desk-abc123", i, topic, sizeof(topic)));
    assert(mqttDiscoveryPayload("standing-desk-abc123", "home/office/desk", i, config, sizeof(config)));
    printf("{\"topic\":\"%s\",\"config\":%s}\n", topic, config);
    char longest[97]; memset(longest, 'x', 96); longest[96] = 0;
    assert(mqttDiscoveryPayload("standing-desk-abc123", longest, i, config, sizeof(config)));
    assert(!mqttDiscoveryPayload("standing-desk-abc123", longest, i, config, 10));
  }
  assert(!mqttDiscoveryTopic("bad/id", 0, topic, sizeof(topic)));
  assert(!mqttDiscoveryPayload("id", "bad/\"topic", 0, config, sizeof(config)));
  assert(!mqttDiscoveryPayload("id", "desk", mqttEntityCount, config, sizeof(config)));
}
'''
with tempfile.TemporaryDirectory(prefix="discovery-test-", dir=root / ".pio") as directory:
    binary = str(Path(directory) / "mqtt_discovery_test")
    subprocess.run(["g++", "-x", "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", "-I", str(root / "include"), "-", "-o", binary],
                   input=code, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, text=True, check=True)
    entities = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(entities) == 10
    assert len({entity["config"]["unique_id"] for entity in entities}) == 10
    assert len({entity["topic"] for entity in entities}) == 10
    for entity in entities:
        config = entity["config"]
        assert config["device"]["identifiers"] == ["standing-desk-abc123"]
        assert config["availability_topic"] == "home/office/desk/availability"
        assert config["qos"] == 0
        if "command_topic" in config:
            assert config["command_topic"].startswith("home/office/desk/command/")
            assert config["retain"] is False
    target = next(entity["config"] for entity in entities if "/number/" in entity["topic"])
    assert (target["min"], target["max"], target["step"]) == (68, 120, 1)
print("All Home Assistant discovery tests passed.")

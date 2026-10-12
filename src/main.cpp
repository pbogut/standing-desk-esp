#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#include "desk_protocol.h"
#include "secrets.h"

static_assert(sizeof(apPassword) >= 9 && sizeof(apPassword) <= 64,
              "AP password must contain 8 to 63 characters");

DeskMonitor desk;
ESP8266WebServer server(80);
uint32_t uartOverruns = 0, uartErrors = 0;

const char page[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk height monitor</title>
<style>
body{font:16px system-ui;max-width:44rem;margin:2rem auto;padding:0 1rem}
#height{font-size:3rem;margin:1rem 0}pre{white-space:pre-wrap;overflow-wrap:anywhere}
</style>
<h1>Desk height monitor</h1>
<p>Receive-only UART, 9600 8N1. Commands are disabled.</p>
<div id="height">No reading</div><p id="state">Connecting...</p>
<details open><summary>UART diagnostics</summary><pre id="diagnostics"></pre></details>
<script>
const height=document.getElementById('height');
const state=document.getElementById('state');
const diagnostics=document.getElementById('diagnostics');
async function poll(){
  const abort=new AbortController();
  const timeout=setTimeout(()=>abort.abort(),2000);
  try{
    const response=await fetch('/api/status',{cache:'no-store',signal:abort.signal});
    if(!response.ok)throw new Error('HTTP '+response.status);
    const data=await response.json();
    height.textContent=data.fresh ? data.height_cm+' cm' : 'No fresh reading';
    state.textContent=data.state.replaceAll('_',' ');
    diagnostics.textContent=JSON.stringify(data,null,2);
  }catch(error){
    height.textContent='No fresh reading';
    state.textContent='Monitor unreachable';
  }finally{
    clearTimeout(timeout);
    setTimeout(poll,500);
  }
}
poll();
</script></html>)HTML";

void receiveDesk() {
  // Bound each pass so continuous noise cannot starve the network/watchdog.
  for (unsigned count = 0; count < 256 && Serial.available(); ++count) {
    const int byte = Serial.read();
    if (byte >= 0) desk.feed(static_cast<uint8_t>(byte), millis());
  }
  if (Serial.hasOverrun()) ++uartOverruns;
  if (Serial.hasRxError()) ++uartErrors;
}

String heightString() {
  return String(desk.heightHundredths / 100.0, 2);
}

void sendStatus() {
  receiveDesk();
  const uint32_t now = millis();
  const bool fresh = desk.fresh(now);
  const char *state = fresh ? "fresh" : desk.validFrames == 0 ? "waiting_for_frames"
      : !desk.displayNumeric ? "unsupported_display" : "stale";
  String json;
  json.reserve(640);
  json = F("{\"state\":\"");
  json += state;
  json += F("\",\"fresh\":");
  json += fresh ? "true" : "false";
  json += F(",\"height_cm\":");
  json += fresh ? heightString() : "null";
  json += F(",\"last_known_height_cm\":");
  json += desk.hasHeight ? heightString() : "null";
  json += F(",\"height_age_ms\":");
  json += desk.hasHeight ? String(uint32_t(now - desk.lastHeightMs)) : "null";
  json += F(",\"last_frame_age_ms\":");
  json += desk.hasFrame ? String(uint32_t(now - desk.lastFrameMs)) : "null";
  json += F(",\"last_frame\":");
  if (desk.hasFrame) {
    json += '"';
    for (unsigned i = 0; i < 6; ++i) {
      char hex[4];
      snprintf(hex, sizeof(hex), "%02X", desk.lastFrame[i]);
      if (i) json += ' ';
      json += hex;
    }
    json += '"';
  } else {
    json += "null";
  }
  json += F(",\"bytes_received\":"); json += desk.bytes;
  json += F(",\"valid_frames\":"); json += desk.validFrames;
  json += F(",\"checksum_failures\":"); json += desk.checksumFailures;
  json += F(",\"invalid_displays\":"); json += desk.invalidDisplays;
  json += F(",\"partial_timeouts\":"); json += desk.partialTimeouts;
  json += F(",\"uart_overruns\":"); json += uartOverruns;
  json += F(",\"uart_errors\":"); json += uartErrors;
  json += F(",\"stale_after_ms\":"); json += DeskMonitor::staleMs;
  json += F(",\"uptime_ms\":"); json += now;
  json += F(",\"commands_enabled\":false}");
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

void setup() {
  // ROM boot output happens before setup. Nothing here can suppress it.
  pinMode(1, INPUT);
  Serial.setDebugOutput(false);
  Serial.setRxBufferSize(1024);
  Serial.begin(9600, SERIAL_8N1, SERIAL_RX_ONLY);
  pinMode(1, INPUT);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  const String ssid = "standing-desk-" + String(ESP.getChipId(), HEX);
  if (!WiFi.softAP(ssid.c_str(), apPassword)) {
    // Remain receive-only if the AP cannot start.
    return;
  }
  server.on("/", HTTP_GET, []() {
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.send_P(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  receiveDesk();
  server.handleClient();
  receiveDesk();
  yield();
}

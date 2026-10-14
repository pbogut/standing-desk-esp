#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>

#include "desk_protocol.h"
#include "panel_bridge.h"
#include "secrets.h"
#include "target_move.h"
#include "wifi_credential_store.h"

static_assert(sizeof(apPassword) >= 9 && sizeof(apPassword) <= 64,
              "AP password must contain 8 to 63 characters");

DeskMonitor desk;
PanelBridge panel;
TargetMove target;
SoftwareSerial statusSerial;
SoftwareSerial panelSerial;
ESP8266WebServer server(80);
uint32_t uartOverruns = 0, panelUartOverruns = 0;
WifiCredentials wifiCredentials;
WifiCredentialStore wifiStore;
WifiConnection wifiConnection;
String deviceName;
bool wifiStorageReady = false, mdnsRunning = false;
enum class WifiChange { None, Connect, Forget };
WifiChange wifiChange = WifiChange::None;
uint32_t wifiChangeMs = 0;

String wifiStatusJson();

constexpr int8_t statusRxPin = D5;  // GPIO14, tap on controller -> panel status.
constexpr int8_t panelRxPin = D6;   // GPIO12, panel side of split command wire.
constexpr int8_t deskTxPin = D7;    // GPIO13, controller side of command wire.

const char page[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk panel bridge</title>
<style>
body{font:16px system-ui;max-width:44rem;margin:2rem auto;padding:0 1rem}
#height{font-size:3rem;margin:1rem 0}pre{white-space:pre-wrap;overflow-wrap:anywhere}
button{font:inherit;padding:.7rem 1rem;margin:.2rem}
button[data-direction]{touch-action:none;user-select:none}
</style>
<h1>Desk panel bridge</h1>
<p>Physical panel buttons take priority over web controls.</p>
<div id="height">No reading</div><p id="state">Connecting...</p>
<p id="panel">Waiting for panel traffic...</p>
<div aria-label="Saved heights">
<button data-preset="1" disabled>Preset 1</button>
<button data-preset="2" disabled>Preset 2</button>
<button data-preset="3" disabled>Preset 3</button>
</div>
<div aria-label="Manual movement">
<button data-direction="up" disabled>Hold Up</button>
<button data-direction="down" disabled>Hold Down</button>
<button id="release" disabled>Release</button>
</div>
<p>Release ends an ESP button press, not automatic preset travel.
Press a preset again or any physical button to interrupt preset travel.</p>
<p id="control-state" role="status">Connecting controls...</p>
<form id="target-form">
<label for="target-height">Target height, cm</label>
<input id="target-height" type="number" min="68" max="120" step="1" required disabled>
<button id="target-submit" type="submit" disabled>Move to height</button>
<button id="target-cancel" type="button" disabled>Cancel automatic move</button>
</form>
<p id="target-state" role="status">Automatic movement requires a fresh height display.</p>
<details><summary>Wi-Fi settings</summary>
<p id="wifi-state" role="status">Loading Wi-Fi status...</p>
<p><a id="wifi-address" hidden>Open desk on home Wi-Fi</a></p>
<form id="wifi-form">
<p><label for="wifi-ssid">Home Wi-Fi SSID</label><br>
<input id="wifi-ssid" type="text" maxlength="32" required autocomplete="off" disabled></p>
<p><label for="wifi-password">Wi-Fi password</label><br>
<input id="wifi-password" type="password" maxlength="63" autocomplete="off" disabled></p>
<p>Use a 2.4 GHz network. Leave the password blank for an open network.
Saving replaces the previous settings; the saved password is never shown.</p>
<button id="wifi-save" type="submit" disabled>Save and connect</button>
<button id="wifi-forget" type="button" disabled>Forget network</button>
</form>
<p id="wifi-message" role="status"></p>
</details>
<details open><summary>UART diagnostics</summary><pre id="diagnostics"></pre></details>
<script>
const height=document.getElementById('height');
const state=document.getElementById('state');
const diagnostics=document.getElementById('diagnostics');
const panel=document.getElementById('panel');
const controlState=document.getElementById('control-state');
const releaseButton=document.getElementById('release');
const targetForm=document.getElementById('target-form');
const targetHeight=document.getElementById('target-height');
const targetSubmit=document.getElementById('target-submit');
const targetCancel=document.getElementById('target-cancel');
const targetState=document.getElementById('target-state');
const wifiForm=document.getElementById('wifi-form');
const wifiSsid=document.getElementById('wifi-ssid');
const wifiPassword=document.getElementById('wifi-password');
const wifiSave=document.getElementById('wifi-save');
const wifiForget=document.getElementById('wifi-forget');
const wifiState=document.getElementById('wifi-state');
const wifiAddress=document.getElementById('wifi-address');
const wifiMessage=document.getElementById('wifi-message');
const presetButtons=[...document.querySelectorAll('[data-preset]')];
const moveButtons=[...document.querySelectorAll('[data-direction]')];
let available=false, presetPending=false, hold=null, remoteId=null;
let targetReady=false, targetActive=false, targetId=null, targetRequest=null;
let wifiReady=false, wifiSaved=false, wifiEdited=false, wifiPending=false;
function updateWifiControls(){
  wifiSsid.disabled=wifiPassword.disabled=wifiSave.disabled=wifiPending || !wifiReady;
  wifiForget.disabled=wifiPending || !wifiReady || !wifiSaved;
}
function showWifi(data){
  wifiReady=data.storage_ready;
  wifiSaved=data.configured;
  if(!wifiEdited)wifiSsid.value=data.ssid;
  wifiAddress.hidden=!data.connected;
  if(data.connected){
    wifiAddress.href='http://'+data.ip+'/';
    wifiAddress.textContent='http://'+data.ip+'/';
  }
  if(data.state==='applying')wifiState.textContent='Applying Wi-Fi settings...';
  else if(data.connected)wifiState.textContent='Connected to '+data.station_ssid
    +'. Switch to your home Wi-Fi to open the address above.'
    +(data.ap_active ? ' Backup Wi-Fi closes after 30 seconds connected.' : '');
  else if(data.ap_active)wifiState.textContent='Backup network: '+data.ap_ssid
    +' at http://'+data.ap_ip+'/'
    +(data.configured ? '. Still trying your home Wi-Fi.' : '. Enter your home Wi-Fi settings below.');
  else wifiState.textContent='Connecting to '+data.ssid+'. Backup Wi-Fi in '
    +Math.ceil(data.fallback_remaining_ms/1000)+' seconds.';
  if(!wifiReady)wifiState.textContent+=' Settings storage unavailable.';
  updateWifiControls();
}
wifiSsid.addEventListener('input',()=>{wifiEdited=true;});
wifiPassword.addEventListener('input',()=>{wifiEdited=true;});
function updateControls(){
  const busy=presetPending || targetActive || targetRequest!==null;
  for(const button of presetButtons)button.disabled=!available || busy || hold!==null;
  for(const button of moveButtons)button.disabled=hold!==null
    ? button.dataset.direction!==hold.direction : !available || busy;
  releaseButton.disabled=hold===null && remoteId===null;
  targetHeight.disabled=targetSubmit.disabled=!targetReady || busy || hold!==null;
  targetCancel.disabled=targetRequest===null && !targetActive;
}
async function command(path, args){
  const abort=new AbortController();
  const timeout=setTimeout(()=>abort.abort(),1000);
  try{
    const response=await fetch('/api/'+path,{method:'POST',
      body:new URLSearchParams(args),signal:abort.signal,keepalive:true,cache:'no-store'});
    const data=await response.json();
    if(!response.ok)throw new Error(data.error || 'HTTP '+response.status);
    return data;
  }finally{clearTimeout(timeout);}
}
wifiForm.addEventListener('submit',async event=>{
  event.preventDefault();
  if(wifiPending || !wifiReady)return;
  wifiPending=true;
  updateWifiControls();
  try{
    const data=await command('wifi',{ssid:wifiSsid.value,password:wifiPassword.value});
    wifiPassword.value='';
    wifiEdited=false;
    showWifi(data);
    wifiMessage.textContent='Saved. Connecting to your home Wi-Fi.';
  }catch(error){wifiMessage.textContent='Could not save Wi-Fi: '+error.message;}
  finally{wifiPending=false;updateWifiControls();}
});
wifiForget.addEventListener('click',async()=>{
  if(wifiPending || !wifiReady || !wifiSaved)return;
  wifiPending=true;
  updateWifiControls();
  try{
    const data=await command('wifi/forget',{});
    wifiPassword.value='';
    wifiEdited=false;
    showWifi(data);
    wifiMessage.textContent='Network forgotten. Join '+data.ap_ssid+' and open http://192.168.4.1/.';
  }catch(error){wifiMessage.textContent='Could not forget Wi-Fi: '+error.message;}
  finally{wifiPending=false;updateWifiControls();}
});
async function releaseCommand(id){
  try{await command('release',{id});}
  catch(error){if(hold===null)controlState.textContent='Release request failed: '+error.message;}
}
function endHold(message='Button released'){
  const current=hold;
  if(current===null)return;
  hold=null;
  current.pressed=false;
  clearTimeout(current.timer);
  available=false;
  controlState.textContent=message;
  updateControls();
  if(current.id!==null)releaseCommand(current.id);
}
async function renewHold(current){
  if(hold!==current)return;
  try{
    await command('move',{direction:current.direction,id:current.id});
    if(hold===current)current.timer=setTimeout(()=>renewHold(current),100);
  }catch(error){if(hold===current)endHold('Hold ended: '+error.message);}
}
async function beginHold(direction){
  if(hold!==null || presetPending || targetActive || targetRequest!==null || !available)return;
  const current={direction,id:null,pressed:true,timer:null};
  hold=current;
  available=false;
  controlState.textContent='Starting '+direction+'...';
  updateControls();
  try{
    const data=await command('move',{direction});
    current.id=data.command_id;
    if(hold!==current || !current.pressed){releaseCommand(current.id);return;}
    controlState.textContent='Holding '+direction;
    current.timer=setTimeout(()=>renewHold(current),100);
  }catch(error){if(hold===current)endHold('Could not start: '+error.message);}
}
for(const button of presetButtons)button.addEventListener('click',async()=>{
  if(!available || presetPending || hold!==null || targetActive || targetRequest!==null)return;
  presetPending=true;
  available=false;
  updateControls();
  try{
    await command('preset',{number:button.dataset.preset});
    controlState.textContent='Preset '+button.dataset.preset+' pressed';
  }catch(error){controlState.textContent='Preset request failed: '+error.message;}
  finally{presetPending=false;updateControls();}
});
async function cancelAutomatic(id){
  try{
    await command('target/cancel',{id});
    targetActive=false;
    available=targetReady=false;
    targetState.textContent='Automatic move cancelled';
  }catch(error){controlState.textContent='Cancel request failed: '+error.message;}
  updateControls();
}
targetForm.addEventListener('submit',async event=>{
  event.preventDefault();
  if(!targetReady || targetActive || targetRequest!==null || presetPending || hold!==null)return;
  const current={cancelled:false};
  targetRequest=current;
  available=targetReady=false;
  const requestedHeight=targetHeight.value;
  targetState.textContent='Requesting '+requestedHeight+' cm...';
  updateControls();
  try{
    const data=await command('target',{height_cm:requestedHeight});
    targetId=data.target_command_id;
    targetActive=data.target_active;
    if(current.cancelled && targetActive)await cancelAutomatic(targetId);
    else targetState.textContent=targetActive ? 'Moving to '+requestedHeight+' cm' : 'Target already reached';
  }catch(error){controlState.textContent='Target request failed: '+error.message;}
  finally{targetRequest=null;updateControls();}
});
targetCancel.addEventListener('click',()=>{
  if(targetRequest!==null){
    targetRequest.cancelled=true;
    targetState.textContent='Cancelling automatic move...';
  }else if(targetActive && targetId!==null)cancelAutomatic(targetId);
});
for(const button of moveButtons){
  button.addEventListener('pointerdown',event=>{
    if(event.button!==0)return;
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    beginHold(button.dataset.direction);
  });
  button.addEventListener('pointerup',()=>endHold());
  button.addEventListener('pointercancel',()=>endHold());
  button.addEventListener('lostpointercapture',()=>endHold());
  button.addEventListener('keydown',event=>{
    if(event.key!==' ' && event.key!=='Enter')return;
    event.preventDefault();
    if(!event.repeat)beginHold(button.dataset.direction);
  });
  button.addEventListener('keyup',event=>{
    if(event.key===' ' || event.key==='Enter'){event.preventDefault();endHold();}
  });
}
releaseButton.addEventListener('click',()=>{
  if(hold!==null)endHold();
  else if(remoteId!==null){releaseCommand(remoteId);available=false;updateControls();}
});
window.addEventListener('blur',()=>endHold());
window.addEventListener('pagehide',()=>endHold());
document.addEventListener('visibilitychange',()=>{if(document.hidden)endHold();});
async function poll(){
  const abort=new AbortController();
  const timeout=setTimeout(()=>abort.abort(),2000);
  try{
    const response=await fetch('/api/status',{cache:'no-store',signal:abort.signal});
    if(!response.ok)throw new Error('HTTP '+response.status);
    const data=await response.json();
    const shownHeight=data.fresh ? data.height_cm : data.last_known_height_cm;
    height.textContent=shownHeight!==null ? shownHeight+' cm' : 'No reading';
    state.textContent=(!data.fresh && shownHeight!==null ? 'Last known height. ' : '')
      +data.state.replaceAll('_',' ');
    panel.textContent=data.panel_fresh ? 'Panel traffic received' : 'No recent panel traffic';
    diagnostics.textContent=JSON.stringify(data,null,2);
    if(data.wifi)showWifi(data.wifi);
    available=data.remote_ready;
    targetReady=data.target_ready;
    targetActive=data.target_active;
    targetId=data.target_command_id;
    targetState.textContent=targetActive
      ? 'Moving '+data.target_direction+' to '+data.target_height_cm+' cm'
      : data.target_state==='idle' ? 'Automatic movement requires a fresh height display.'
      : data.target_state+'. '+data.target_reason.replaceAll('_',' ');
    if(controlState.textContent==='Connecting controls...'
       || controlState.textContent==='Waiting for idle panel')
      controlState.textContent=available ? 'Controls ready' : 'Waiting for idle panel';
    remoteId=data.remote_button==='idle' ? null : data.remote_command_id;
    if(hold!==null && hold.id!==null && data.remote_command_id===hold.id
       && data.remote_button==='idle')endHold('Hold ended or physical panel took priority');
    updateControls();
  }catch(error){
    height.textContent='No fresh reading';
    state.textContent='Monitor unreachable';
    panel.textContent='Panel status unavailable';
    available=false;
    targetReady=false;
    targetState.textContent='Monitor unreachable. Automatic movement is managed by the ESP.';
    remoteId=null;
    endHold('Monitor unreachable');
    controlState.textContent='Monitor unreachable';
    wifiReady=false;
    wifiAddress.hidden=true;
    wifiState.textContent='Monitor unreachable. Rejoin home Wi-Fi or the backup network.';
    updateWifiControls();
    updateControls();
  }finally{
    clearTimeout(timeout);
    setTimeout(poll,500);
  }
}
poll();
</script></html>)HTML";

void serviceSerial() {
  if (panelSerial.overflow()) {
    ++panelUartOverruns;
    panel.inputOverflow(millis());
  }
  if (statusSerial.overflow()) {
    ++uartOverruns;
    desk.inputOverflow();
    target.stop("status_uart_overflow", panel, millis());
  }
  // Check every display frame before transmitting another automatic command.
  // A bad display followed by a valid one in the same batch must still cancel.
  unsigned statusBytes = 0;
  for (; statusBytes < 256 && statusSerial.available(); ++statusBytes) {
    const int byte = statusSerial.read();
    if (byte >= 0) {
      desk.feed(static_cast<uint8_t>(byte), millis());
      target.update(desk, panel, millis());
    }
  }
  // Bytes arriving after an empty-buffer observation are normal traffic.
  // Only an exhausted processing budget with bytes left indicates a backlog.
  if (statusBytes == 256 && statusSerial.available())
    target.stop("status_backlog", panel, millis());
  if (statusSerial.overflow()) {
    ++uartOverruns;
    desk.inputOverflow();
    target.stop("status_uart_overflow", panel, millis());
  }
  target.update(desk, panel, millis());
  panel.forward(panelSerial, millis());
  if (panelSerial.overflow()) {
    ++panelUartOverruns;
    panel.inputOverflow(millis());
  }
  target.update(desk, panel, millis());
}

const char *remoteButtonName() {
  switch (panel.commandButton) {
    case 0x02: return "preset_1";
    case 0x04: return "preset_2";
    case 0x08: return "preset_3";
    case 0x20: return "up";
    case 0x40: return "down";
    default: return "idle";
  }
}

void appendTargetStatus(String &json, uint32_t now) {
  json += F(",\"automatic_height_valid\":");
  json += desk.automaticHeightFresh(now) ? "true" : "false";
  json += F(",\"target_active\":"); json += target.active() ? "true" : "false";
  json += F(",\"target_ready\":");
  json += !target.active() && desk.automaticHeightFresh(now) && panel.ready(now) ? "true" : "false";
  json += F(",\"target_command_id\":"); json += target.commandId;
  json += F(",\"target_height_cm\":");
  json += target.state == TargetMove::State::Idle ? String("null") : String(target.targetHundredths / 100);
  json += F(",\"target_direction\":\"");
  json += !target.active() ? "idle" : target.button == 0x20 ? "up" : "down";
  json += F("\",\"target_state\":\""); json += target.stateName();
  json += F("\",\"target_reason\":\""); json += target.reason; json += '"';
  json += F(",\"target_progress_timeout_ms\":"); json += TargetMove::progressTimeoutMs;
  json += F(",\"target_max_travel_ms\":"); json += PanelBridge::maxTargetMs;
  json += F(",\"automatic_height_timeout_ms\":"); json += DeskMonitor::automaticStaleMs;
}

void commandResult(int code, const char *error = nullptr) {
  String json = F("{\"command_id\":");
  json += panel.commandId;
  appendTargetStatus(json, millis());
  if (error) {
    json += F(",\"error\":\"");
    json += error;
    json += '"';
  }
  json += '}';
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, F("application/json"), json);
}

bool readCommandId(uint32_t &id) {
  const String text = server.arg("id");
  if (text.length() == 0 || text.length() > 10) return false;
  id = 0;
  for (unsigned i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    const unsigned digit = text[i] - '0';
    if (id > (UINT32_MAX - digit) / 10) return false;
    id = id * 10 + digit;
  }
  return id != 0;
}

void pressPreset() {
  serviceSerial();
  const String number = server.arg("number");
  if (number != "1" && number != "2" && number != "3") {
    commandResult(400, "number must be 1, 2 or 3");
    return;
  }
  const uint8_t button = 1 << (number[0] - '0');
  if (!panel.start(button, false, millis())) {
    commandResult(409, "Panel must be idle; command busy or release gap active");
    return;
  }
  commandResult(200);
}

void moveDesk() {
  serviceSerial();
  const String direction = server.arg("direction");
  if (direction != "up" && direction != "down") {
    commandResult(400, "direction must be up or down");
    return;
  }
  const uint8_t button = direction == "up" ? 0x20 : 0x40;
  bool accepted;
  if (server.hasArg("id")) {
    uint32_t id;
    if (!readCommandId(id)) {
      commandResult(400, "id must be a nonzero 32-bit unsigned integer");
      return;
    }
    accepted = panel.renew(id, button, millis());
  } else {
    accepted = panel.start(button, true, millis());
  }
  commandResult(accepted ? 200 : 409, accepted ? nullptr
      : "Hold expired, panel active, command busy or release gap active");
}

void releaseDesk() {
  serviceSerial();
  uint32_t id;
  if (!readCommandId(id)) {
    commandResult(400, "id must be a nonzero 32-bit unsigned integer");
    return;
  }
  const bool accepted = panel.release(id, millis());
  target.update(desk, panel, millis());
  commandResult(accepted ? 200 : 409, accepted ? nullptr : "Command id is no longer current");
}

bool readTargetHeight(uint16_t &height) {
  const String text = server.arg("height_cm");
  unsigned length = text.length();
  if (length >= 3 && text[length - 2] == '.' && text[length - 1] == '0') length -= 2;
  if (length == 0 || length > 3) return false;
  height = 0;
  for (unsigned i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    height = height * 10 + text[i] - '0';
  }
  return height >= 68 && height <= 120;
}

void setTarget() {
  serviceSerial();
  uint16_t height;
  if (!readTargetHeight(height)) {
    commandResult(400, "height_cm must be a whole centimetre between 68 and 120");
    return;
  }
  const char *error = target.start(height, desk, panel, millis());
  commandResult(error ? 409 : 200, error);
}

void cancelTarget() {
  serviceSerial();
  uint32_t id;
  if (!readCommandId(id)) {
    commandResult(400, "id must be a nonzero 32-bit unsigned integer");
    return;
  }
  const bool accepted = target.cancel(id, panel, millis());
  commandResult(accepted ? 200 : 409, accepted ? nullptr : "Automatic command id is no longer current");
}

String heightString() {
  return String(desk.heightHundredths / 100.0, 2);
}

void sendStatus() {
  serviceSerial();
  const uint32_t now = millis();
  const bool fresh = desk.fresh(now);
  const char *state = fresh ? "fresh" : desk.validFrames == 0 ? "waiting_for_frames"
      : !desk.displayNumeric ? "unsupported_display" : "stale";
  String json;
  json.reserve(1792);
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
  json += F(",\"panel_bytes_received\":"); json += panel.bytesReceived;
  json += F(",\"panel_bytes_forwarded\":"); json += panel.bytesForwarded;
  json += F(",\"panel_write_failures\":"); json += panel.writeFailures;
  json += F(",\"panel_uart_overruns\":"); json += panelUartOverruns;
  json += F(",\"panel_bytes_suppressed\":"); json += panel.bytesSuppressed;
  json += F(",\"esp_bytes_sent\":"); json += panel.espBytesSent;
  json += F(",\"panel_invalid_frames\":"); json += panel.invalidFrames;
  json += F(",\"panel_partial_timeouts\":"); json += panel.partialTimeouts;
  json += F(",\"physical_overrides\":"); json += panel.physicalOverrides;
  json += F(",\"remote_button\":\""); json += remoteButtonName(); json += '"';
  json += F(",\"remote_command_id\":"); json += panel.commandId;
  json += F(",\"remote_ready\":"); json += panel.ready(now) ? "true" : "false";
  appendTargetStatus(json, now);
  json += F(",\"hold_lease_ms\":"); json += PanelBridge::holdLeaseMs;
  json += F(",\"max_hold_ms\":"); json += PanelBridge::maxHoldMs;
  json += F(",\"preset_press_ms\":"); json += PanelBridge::presetMs;
  json += F(",\"release_gap_ms\":"); json += PanelBridge::releaseGapMs;
  json += F(",\"panel_age_ms\":");
  json += panel.bytesReceived ? String(uint32_t(now - panel.lastByteMs)) : "null";
  json += F(",\"panel_fresh\":"); json += panel.fresh(now) ? "true" : "false";
  json += F(",\"stale_after_ms\":"); json += DeskMonitor::staleMs;
  json += F(",\"uptime_ms\":"); json += now;
  json += F(",\"passthrough_enabled\":true,\"commands_enabled\":true,\"wifi\":");
  json += wifiStatusJson();
  json += '}';
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

struct DeskWifiNetwork {
  bool isConnected() const {
    return WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP()) != 0;
  }
  void connect(bool keepAp) {
    WiFi.mode(keepAp ? WIFI_AP_STA : WIFI_STA);
    WiFi.disconnect(false);
    WiFi.hostname(deviceName);
    WiFi.begin(wifiCredentials.ssid, wifiCredentials.password);
  }
  void disconnect() {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
  }
  bool startAp(bool station) {
    WiFi.mode(station ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    return WiFi.softAP(deviceName.c_str(), apPassword);
  }
  void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
  }
} wifiNetwork;

String jsonQuoted(const String &text) {
  String quoted = "\"";
  for (unsigned i = 0; i < text.length(); ++i) {
    const uint8_t byte = static_cast<uint8_t>(text[i]);
    if (byte == '"' || byte == '\\') quoted += '\\';
    if (byte < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      quoted += escaped;
    } else quoted += static_cast<char>(byte);
  }
  quoted += '"';
  return quoted;
}

String wifiStatusJson() {
  String json;
  json.reserve(512);
  json = F("{\"configured\":"); json += wifiCredentials.valid() ? "true" : "false";
  json += F(",\"storage_ready\":"); json += wifiStorageReady ? "true" : "false";
  json += F(",\"ssid\":"); json += jsonQuoted(wifiCredentials.ssid);
  json += F(",\"password_saved\":"); json += wifiCredentials.password[0] ? "true" : "false";
  json += F(",\"state\":");
  json += jsonQuoted(wifiChange != WifiChange::None ? "applying" : wifiConnection.stateName());
  json += F(",\"connected\":"); json += wifiConnection.connected ? "true" : "false";
  json += F(",\"station_ssid\":");
  json += jsonQuoted(wifiConnection.connected ? WiFi.SSID() : String(""));
  json += F(",\"ip\":");
  json += jsonQuoted(wifiConnection.connected ? WiFi.localIP().toString() : String(""));
  json += F(",\"hostname\":"); json += jsonQuoted(deviceName + ".local");
  json += F(",\"ap_active\":"); json += wifiConnection.apActive ? "true" : "false";
  json += F(",\"ap_ssid\":"); json += jsonQuoted(deviceName);
  json += F(",\"ap_ip\":");
  json += jsonQuoted(wifiConnection.apActive ? WiFi.softAPIP().toString() : String(""));
  json += F(",\"fallback_remaining_ms\":"); json += wifiConnection.fallbackRemaining(millis());
  json += F(",\"ap_grace_ms\":"); json += WifiConnection::apGraceMs;
  json += '}';
  return json;
}

void sendWifiStatus() {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), wifiStatusJson());
}

void saveWifiSettings() {
  const String ssid = server.arg("ssid"), password = server.arg("password");
  WifiCredentials saved;
  if (!server.hasArg("ssid") || !server.hasArg("password")
      || ssid.length() != strlen(ssid.c_str()) || password.length() != strlen(password.c_str())
      || !saved.set(ssid.c_str(), password.c_str())) {
    server.send(400, F("application/json"),
        F("{\"error\":\"SSID must be 1 to 32 bytes; password must be empty or 8 to 63 bytes\"}"));
    return;
  }
  if (!wifiStorageReady || !wifiStore.save(LittleFS, saved)) {
    server.send(500, F("application/json"), F("{\"error\":\"Could not save Wi-Fi settings\"}"));
    return;
  }
  wifiCredentials = saved;
  wifiChange = WifiChange::Connect;
  wifiChangeMs = millis();
  sendWifiStatus();
}

void forgetWifiSettings() {
  if (!wifiStorageReady || !wifiStore.forget(LittleFS)) {
    server.send(500, F("application/json"), F("{\"error\":\"Could not remove Wi-Fi settings\"}"));
    return;
  }
  wifiCredentials = WifiCredentials();
  wifiChange = WifiChange::Forget;
  wifiChangeMs = millis();
  sendWifiStatus();
}

void serviceWifi() {
  const uint32_t now = millis();
  if (wifiChange != WifiChange::None) {
    // Let the HTTP reply leave before changing its network interface.
    if (uint32_t(now - wifiChangeMs) < 500) return;
    if (mdnsRunning) { MDNS.end(); mdnsRunning = false; }
    wifiConnection.configure(wifiChange == WifiChange::Connect, wifiNetwork, now);
    wifiChange = WifiChange::None;
  }
  wifiConnection.update(wifiNetwork, now);
  if (wifiConnection.connected && !mdnsRunning) {
    mdnsRunning = MDNS.begin(deviceName.c_str());
    if (mdnsRunning) MDNS.addService("http", "tcp", 80);
  } else if (!wifiConnection.connected && mdnsRunning) {
    MDNS.end();
    mdnsRunning = false;
  }
  if (mdnsRunning) MDNS.update();
}

void setup() {
  // USB UART is independent of all three desk pins, including ROM boot output.
  Serial.setDebugOutput(false);
  Serial.begin(115200);
  // Hold the controller command input at UART idle during network startup.
  digitalWrite(deskTxPin, HIGH);
  pinMode(deskTxPin, OUTPUT);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  deviceName = "standing-desk-" + String(ESP.getChipId(), HEX);
  wifiStorageReady = LittleFS.begin();
  if (wifiStorageReady) wifiStore.load(LittleFS, wifiCredentials);
  wifiConnection.configure(wifiCredentials.valid(), wifiNetwork, millis());
  server.on("/", HTTP_GET, []() {
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.send_P(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/preset", HTTP_POST, pressPreset);
  server.on("/api/move", HTTP_POST, moveDesk);
  server.on("/api/release", HTTP_POST, releaseDesk);
  server.on("/api/target", HTTP_POST, setTarget);
  server.on("/api/target/cancel", HTTP_POST, cancelTarget);
  server.on("/api/wifi", HTTP_GET, sendWifiStatus);
  server.on("/api/wifi", HTTP_POST, saveWifiSettings);
  server.on("/api/wifi/forget", HTTP_POST, forgetWifiSettings);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  // Start reception after network setup so old button presses are not queued
  // during startup. Both receivers remain active during software UART TX.
  statusSerial.begin(9600, SWSERIAL_8N1, statusRxPin, -1, false, 256, 2560);
  panelSerial.begin(9600, SWSERIAL_8N1, panelRxPin, deskTxPin, false, 256, 2560);
  panelSerial.enableIntTx(true);
  Serial.println(F("Desk panel bridge ready; Wi-Fi setup/status at /api/wifi"));
}

void loop() {
  serviceSerial();
  server.handleClient();
  serviceSerial();
  serviceWifi();
  yield();
}

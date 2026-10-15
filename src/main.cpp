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
#include "mqtt_service.h"
#include "wifi_credential_store.h"
#include "wifi_scan.h"

static_assert(sizeof(apPassword) >= 9 && sizeof(apPassword) <= 64,
              "AP password must contain 8 to 63 characters");

DeskMonitor desk;
PanelBridge panel;
TargetMove target;
DeskMqtt mqtt(desk, panel, target);
MqttSettings mqttSettings;
MqttSettingsStore mqttStore;
SoftwareSerial statusSerial;
SoftwareSerial panelSerial;
ESP8266WebServer server(80);
uint32_t uartOverruns = 0, panelUartOverruns = 0;
WifiCredentials wifiCredentials;
WifiCredentialStore wifiStore;
WifiConnection wifiConnection;
WifiScan wifiScan;
bool wifiScanCleanupPending = false;
String deviceName;
bool wifiStorageReady = false, mdnsRunning = false;
enum class WifiChange { None, Connect, Forget };
WifiChange wifiChange = WifiChange::None;
uint32_t wifiChangeMs = 0;

String wifiStatusJson();
String mqttStatusJson();
String jsonQuoted(const String &text);

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
<p id="control-ready" role="status">Connecting controls...</p>
<p id="control-state" role="status"></p>
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
<button id="wifi-scan" type="button" disabled>Scan networks</button>
<p id="wifi-scan-state" role="status"></p>
<p><label for="wifi-networks">Nearby networks</label><br>
<select id="wifi-networks" disabled><option value="">Scan to find networks</option></select></p>
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
<details><summary>MQTT / Home Assistant</summary>
<p id="mqtt-state" role="status">Loading MQTT status...</p>
<form id="mqtt-form">
<p><label><input id="mqtt-enabled" type="checkbox" disabled> Enable MQTT</label></p>
<p><label for="mqtt-host">Broker IP or hostname</label><br>
<input id="mqtt-host" maxlength="64" placeholder="192.168.1.50" disabled></p>
<p><label for="mqtt-port">Port</label><br>
<input id="mqtt-port" type="number" min="1" max="65535" value="1883" required disabled></p>
<p><label for="mqtt-user">Username</label><br>
<input id="mqtt-user" maxlength="64" autocomplete="off" disabled></p>
<p><label for="mqtt-password">Password</label><br>
<input id="mqtt-password" type="password" maxlength="128" autocomplete="off" disabled></p>
<p id="mqtt-password-help"></p>
<p><label><input id="mqtt-clear-password" type="checkbox" disabled> Clear saved password</label></p>
<p><label for="mqtt-prefix">Topic prefix</label><br>
<input id="mqtt-prefix" maxlength="96" required disabled></p>
<p>Plain MQTT TCP, usually port 1883. Home Assistant discovery is automatic.
Use letters, digits, /, _, - or . in the prefix, for example home/office/desk.</p>
<button id="mqtt-save" type="submit" disabled>Save and connect</button>
</form>
<p id="mqtt-message" role="status"></p>
</details>
<details open><summary>UART diagnostics</summary><pre id="diagnostics"></pre></details>
<script>
const height=document.getElementById('height');
const state=document.getElementById('state');
const diagnostics=document.getElementById('diagnostics');
const panel=document.getElementById('panel');
const controlState=document.getElementById('control-state');
const controlReady=document.getElementById('control-ready');
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
const wifiScanButton=document.getElementById('wifi-scan');
const wifiScanState=document.getElementById('wifi-scan-state');
const wifiNetworks=document.getElementById('wifi-networks');
const mqttForm=document.getElementById('mqtt-form');
const mqttState=document.getElementById('mqtt-state');
const mqttMessage=document.getElementById('mqtt-message');
const mqttPasswordHelp=document.getElementById('mqtt-password-help');
const mqttSave=document.getElementById('mqtt-save');
const mqttFields={enabled:document.getElementById('mqtt-enabled'),host:document.getElementById('mqtt-host'),
  port:document.getElementById('mqtt-port'),user:document.getElementById('mqtt-user'),
  password:document.getElementById('mqtt-password'),clear:document.getElementById('mqtt-clear-password'),
  prefix:document.getElementById('mqtt-prefix')};
const presetButtons=[...document.querySelectorAll('[data-preset]')];
const moveButtons=[...document.querySelectorAll('[data-direction]')];
let available=false, presetPending=false, hold=null, remoteId=null;
let targetReady=false, targetActive=false, targetId=null, targetRequest=null;
let wifiReady=false, wifiSaved=false, wifiEdited=false, wifiPending=false;
let wifiScanning=false, wifiCanScan=false, wifiNetworkCount=0, wifiScanTimer=null;
let mqttEdited=false, mqttPending=false, mqttReady=false;
function updateMqttControls(){
  for(const field of Object.values(mqttFields))field.disabled=mqttPending || !mqttReady;
  mqttSave.disabled=mqttPending || !mqttReady;
  mqttFields.host.required=mqttFields.enabled.checked;
}
function showMqtt(data){
  mqttReady=data.storage_ready;
  if(!mqttEdited){
    mqttFields.enabled.checked=data.enabled;
    mqttFields.host.value=data.host;
    mqttFields.port.value=String(data.port);
    mqttFields.user.value=data.username;
    mqttFields.prefix.value=data.topic_prefix;
  }
  mqttPasswordHelp.textContent=data.password_saved
    ? 'A password is saved. Leave blank to keep it, or select Clear saved password.'
    : 'No password saved. Leave username and password blank for an anonymous broker.';
  mqttState.textContent='MQTT: '+data.state.replaceAll('_',' ')
    +(data.error ? '. '+data.error.replaceAll('_',' ') : '')
    +(data.command_error ? '. Last command: '+data.command_error : '');
  updateMqttControls();
}
for(const field of Object.values(mqttFields)){
  field.addEventListener('input',()=>{mqttEdited=true;updateMqttControls();});
  field.addEventListener('change',()=>{mqttEdited=true;updateMqttControls();});
}
function updateWifiControls(){
  wifiSsid.disabled=wifiPassword.disabled=wifiPending || !wifiReady;
  wifiSave.disabled=wifiPending || wifiScanning || !wifiReady;
  wifiForget.disabled=wifiPending || wifiScanning || !wifiReady || !wifiSaved;
  wifiScanButton.disabled=wifiPending || wifiScanning || !wifiCanScan;
  wifiNetworks.disabled=wifiPending || wifiScanning || wifiNetworkCount===0;
}
function showWifi(data){
  wifiReady=data.storage_ready;
  wifiSaved=data.configured;
  wifiCanScan=data.state!=='applying' && (data.connected || data.ap_active);
  if(data.scan_state==='running' && !wifiScanning){
    wifiScanning=true;
    wifiScanState.textContent='Scanning for 2.4 GHz networks...';
    scheduleWifiScanPoll();
  }
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
wifiSsid.addEventListener('input',()=>{wifiEdited=true;wifiNetworks.value='';});
wifiPassword.addEventListener('input',()=>{wifiEdited=true;});
wifiNetworks.addEventListener('change',()=>{
  if(!wifiNetworks.value)return;
  wifiSsid.value=wifiNetworks.value;
  wifiEdited=true;
});
function scheduleWifiScanPoll(){
  if(wifiScanTimer===null)wifiScanTimer=setTimeout(pollWifiScan,500);
}
function showWifiScan(data){
  if(data.state==='running'){
    wifiScanState.textContent='Scanning for 2.4 GHz networks...';
    scheduleWifiScanPoll();
    return;
  }
  wifiScanning=false;
  wifiNetworks.replaceChildren();
  wifiNetworkCount=data.networks.length;
  const placeholder=document.createElement('option');
  placeholder.value='';
  placeholder.textContent=wifiNetworkCount ? 'Select a network' : 'No networks found';
  wifiNetworks.appendChild(placeholder);
  for(const network of data.networks){
    const option=document.createElement('option');
    option.value=network.ssid;
    option.textContent=network.ssid+' | '+network.rssi+' dBm | '
      +(network.secured ? 'Password protected' : 'Open');
    wifiNetworks.appendChild(option);
  }
  wifiScanState.textContent=data.state==='failed' ? data.error || 'Scan failed. Try again.'
    : wifiNetworkCount ? 'Select a network, then enter its password and save.'
    : 'No networks found. Try again or type an SSID manually.';
  updateWifiControls();
}
async function pollWifiScan(){
  clearTimeout(wifiScanTimer);
  wifiScanTimer=null;
  const abort=new AbortController();
  const timeout=setTimeout(()=>abort.abort(),2000);
  try{
    const response=await fetch('/api/wifi/scan',{cache:'no-store',signal:abort.signal});
    const data=await response.json();
    if(!response.ok)throw new Error(data.error || 'HTTP '+response.status);
    showWifiScan(data);
  }catch(error){
    wifiScanning=false;
    wifiScanState.textContent='Could not read scan results: '+error.message;
    updateWifiControls();
  }finally{clearTimeout(timeout);}
}
wifiScanButton.addEventListener('click',async()=>{
  if(wifiPending || wifiScanning || !wifiCanScan)return;
  wifiScanning=true;
  wifiNetworkCount=0;
  wifiNetworks.replaceChildren();
  wifiScanState.textContent='Scanning for 2.4 GHz networks...';
  updateWifiControls();
  try{showWifiScan(await command('wifi/scan',{}));}
  catch(error){
    wifiScanning=false;
    wifiScanState.textContent='Could not start scan: '+error.message;
    updateWifiControls();
  }
});
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
mqttForm.addEventListener('submit',async event=>{
  event.preventDefault();
  if(mqttPending || !mqttReady)return;
  mqttPending=true;
  updateMqttControls();
  try{
    const data=await command('mqtt',{enabled:String(mqttFields.enabled.checked),host:mqttFields.host.value,
      port:mqttFields.port.value,username:mqttFields.user.value,password:mqttFields.password.value,
      clear_password:String(mqttFields.clear.checked),topic_prefix:mqttFields.prefix.value});
    mqttEdited=false;
    mqttFields.password.value='';
    mqttFields.clear.checked=false;
    showMqtt(data);
    mqttMessage.textContent='MQTT settings saved. Check the connection status above.';
  }catch(error){mqttMessage.textContent='Could not save MQTT settings: '+error.message;}
  finally{mqttPending=false;updateMqttControls();}
});
wifiForm.addEventListener('submit',async event=>{
  event.preventDefault();
  if(wifiPending || wifiScanning || !wifiReady)return;
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
  if(wifiPending || wifiScanning || !wifiReady || !wifiSaved)return;
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
    if(data.mqtt)showMqtt(data.mqtt);
    available=data.remote_ready;
    targetReady=data.target_ready;
    targetActive=data.target_active;
    targetId=data.target_command_id;
    targetState.textContent=targetActive
      ? 'Moving '+data.target_direction+' to '+data.target_height_cm+' cm'
      : data.target_state==='idle' ? 'Automatic movement requires a fresh height display.'
      : data.target_state+'. '+data.target_reason.replaceAll('_',' ');
    controlReady.textContent=available ? 'Controls ready'
      : data.remote_button!=='idle' ? 'Remote command active: '+data.remote_button.replaceAll('_',' ')
      : 'Controls busy or waiting for panel release';
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
    controlReady.textContent='Controls unavailable: monitor unreachable';
    wifiReady=false;
    wifiCanScan=false;
    wifiAddress.hidden=true;
    wifiState.textContent='Monitor unreachable. Rejoin home Wi-Fi or the backup network.';
    updateWifiControls();
    mqttReady=false;
    mqttState.textContent='MQTT status unavailable: monitor unreachable';
    updateMqttControls();
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
  json += !target.active() && desk.automaticHeightFresh(now) && panel.targetReady(now) ? "true" : "false";
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
  json.reserve(2560);
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
  json += F(",\"mqtt\":"); json += mqttStatusJson();
  json += '}';
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

String mqttStatusJson() {
  String json;
  json.reserve(640);
  json = F("{\"enabled\":"); json += mqttSettings.enabled ? "true" : "false";
  json += F(",\"storage_ready\":"); json += wifiStorageReady ? "true" : "false";
  json += F(",\"host\":"); json += jsonQuoted(mqttSettings.host);
  json += F(",\"port\":"); json += mqttSettings.port;
  json += F(",\"username\":"); json += jsonQuoted(mqttSettings.username);
  json += F(",\"topic_prefix\":"); json += jsonQuoted(mqttSettings.prefix);
  json += F(",\"password_saved\":"); json += mqttSettings.password[0] ? "true" : "false";
  json += F(",\"connected\":"); json += mqtt.connected() ? "true" : "false";
  json += F(",\"state\":"); json += jsonQuoted(mqtt.stateName());
  json += F(",\"error\":"); json += mqtt.error()[0] ? jsonQuoted(mqtt.error()) : String("null");
  json += F(",\"command_error\":"); json += mqtt.commandError()[0] ? jsonQuoted(mqtt.commandError()) : String("null");
  json += F(",\"commands_accepted\":"); json += mqtt.accepted();
  json += F(",\"commands_rejected\":"); json += mqtt.rejected();
  json += '}';
  return json;
}

void sendMqttStatus() {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), mqttStatusJson());
}

template <unsigned Size>
bool copyMqttField(const String &value, char (&out)[Size]) {
  if (value.length() >= Size || strlen(value.c_str()) != value.length()) return false;
  memcpy(out, value.c_str(), value.length() + 1);
  return true;
}

void saveMqttSettings() {
  const String enabled = server.arg("enabled"), portText = server.arg("port");
  const String clear = server.arg("clear_password"), pass = server.arg("password");
  MqttSettings saved;
  uint32_t port = 0;
  bool validPort = portText.length() > 0 && portText.length() <= 5;
  for (unsigned i = 0; i < portText.length() && validPort; ++i) {
    if (portText[i] < '0' || portText[i] > '9') validPort = false;
    else port = port * 10 + portText[i] - '0';
  }
  saved.enabled = enabled == "true";
  saved.port = port > 65535 ? 0 : port;
  bool valid = (enabled == "true" || enabled == "false") && validPort
      && (clear == "" || clear == "true" || clear == "false")
      && server.hasArg("username") && server.hasArg("password")
      && copyMqttField(server.arg("host"), saved.host)
      && copyMqttField(server.arg("username"), saved.username)
      && copyMqttField(server.arg("topic_prefix"), saved.prefix)
      && copyMqttField(pass, saved.password);
  if (valid && !pass.length() && saved.username[0] && clear != "true")
    strcpy(saved.password, mqttSettings.password);
  if (clear == "true" || !saved.username[0]) saved.password[0] = 0;
  if (!valid || !saved.valid()) {
    server.send(400, F("application/json"),
        F("{\"error\":\"Invalid MQTT settings: use a host/IP, port 1..65535 and topic prefix with letters, digits, /, _, - or .\"}"));
    return;
  }
  if (!wifiStorageReady || !mqttStore.save(LittleFS, saved)) {
    server.send(500, F("application/json"), F("{\"error\":\"Could not save MQTT settings\"}"));
    return;
  }
  mqttSettings = saved;
  mqtt.configure(saved);
  sendMqttStatus();
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
  json += F(",\"scan_state\":"); json += jsonQuoted(wifiScan.stateName());
  json += '}';
  return json;
}

void sendWifiStatus() {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), wifiStatusJson());
}

void finishWifiScan() {
  WiFi.setAutoReconnect(true);
  if (!wifiConnection.configured && !wifiScanCleanupPending) WiFi.mode(WIFI_AP);
  wifiConnection.extendApGrace(millis());
  wifiConnection.update(wifiNetwork, millis());
}

void serviceWifiScan() {
  if (wifiScanCleanupPending && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    wifiScanCleanupPending = false;
    if (!wifiConnection.configured) WiFi.mode(WIFI_AP);
  }
  if (!wifiScan.running()) return;
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (!wifiScan.timedOut(millis())) return;
    wifiScan.fail("Scan timed out");
    wifiScanCleanupPending = true;
  } else if (result < 0) {
    wifiScan.fail("Wi-Fi scan failed");
    WiFi.scanDelete();
  } else {
    for (int i = 0; i < result; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == strlen(ssid.c_str()))
        wifiScan.add(ssid.c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }
    wifiScan.complete();
    WiFi.scanDelete();
  }
  finishWifiScan();
}

void sendWifiScan(int code = 200) {
  String json;
  json.reserve(128 + wifiScan.count * 90);
  json = F("{\"state\":"); json += jsonQuoted(wifiScan.stateName());
  json += F(",\"error\":"); json += wifiScan.error[0] ? jsonQuoted(wifiScan.error) : String("null");
  json += F(",\"networks\":[");
  for (unsigned i = 0; i < wifiScan.count; ++i) {
    const WifiScan::Network &network = wifiScan.networks[i];
    if (i) json += ',';
    json += F("{\"ssid\":"); json += jsonQuoted(network.ssid);
    json += F(",\"rssi\":"); json += String(network.rssi);
    json += F(",\"secured\":"); json += network.secured ? "true" : "false";
    json += '}';
  }
  json += F("]}");
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, F("application/json"), json);
}

void startWifiScan() {
  if (wifiScan.running()) { sendWifiScan(202); return; }
  if (wifiChange != WifiChange::None || WiFi.scanComplete() == WIFI_SCAN_RUNNING
      || (wifiConnection.configured && !wifiConnection.connected && !wifiConnection.apActive)) {
    server.send(409, F("application/json"), F("{\"error\":\"Wi-Fi is busy connecting or scanning; try again shortly\"}"));
    return;
  }
  // The SDK scanner enables STA while preserving an existing AP. Prevent its
  // reconnect machinery and our retry timer from competing with the scan.
  wifiConnection.update(wifiNetwork, millis(), false);
  WiFi.setAutoReconnect(false);
  wifiScanCleanupPending = false;
  wifiScan.begin(millis());
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
    wifiScan.fail("Could not start Wi-Fi scan");
    finishWifiScan();
    sendWifiScan(500);
    return;
  }
  sendWifiScan(202);
}

void saveWifiSettings() {
  if (wifiScan.running()) {
    server.send(409, F("application/json"), F("{\"error\":\"Wait for the Wi-Fi scan to finish\"}"));
    return;
  }
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
  if (wifiScan.running()) {
    server.send(409, F("application/json"), F("{\"error\":\"Wait for the Wi-Fi scan to finish\"}"));
    return;
  }
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
  serviceWifiScan();
  const uint32_t now = millis();
  if (wifiChange != WifiChange::None) {
    // Let the HTTP reply leave before changing its network interface.
    if (uint32_t(now - wifiChangeMs) < 500) return;
    if (mdnsRunning) { MDNS.end(); mdnsRunning = false; }
    wifiConnection.configure(wifiChange == WifiChange::Connect, wifiNetwork, now);
    wifiChange = WifiChange::None;
  }
  wifiConnection.update(wifiNetwork, now, !wifiScan.running());
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
  MqttSettings loadedMqtt;
  if (wifiStorageReady && mqttStore.load(LittleFS, loadedMqtt)) mqttSettings = loadedMqtt;
  else snprintf(mqttSettings.prefix, sizeof(mqttSettings.prefix), "standing-desk/%x", ESP.getChipId());
  mqtt.begin(deviceName.c_str(), mqttSettings);
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
  server.on("/api/wifi/scan", HTTP_POST, startWifiScan);
  server.on("/api/wifi/scan", HTTP_GET, []() { sendWifiScan(); });
  server.on("/api/mqtt", HTTP_GET, sendMqttStatus);
  server.on("/api/mqtt", HTTP_POST, saveMqttSettings);
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
  mqtt.service(wifiConnection.connected && wifiChange == WifiChange::None, !wifiScan.running());
  yield();
}

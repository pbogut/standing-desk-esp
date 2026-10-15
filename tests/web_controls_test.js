const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const source = fs.readFileSync(path.join(__dirname, '../src/main.cpp'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const flush = () => new Promise(resolve => setImmediate(resolve));

function element(dataset = {}) {
  return {
    dataset, textContent: '', value: '', checked: false, disabled: true, listeners: {}, children: [],
    replaceChildren(...nodes) { this.children = nodes; },
    appendChild(node) { this.children.push(node); return node; },
    addEventListener(name, callback) { this.listeners[name] = callback; },
    setPointerCapture() {},
    fire(name, properties = {}) {
      return this.listeners[name]({button: 0, pointerId: 1,
        preventDefault() {}, ...properties});
    }
  };
}

async function page() {
  const elements = Object.fromEntries(
    ['height', 'state', 'panel', 'diagnostics', 'release', 'control-state', 'control-ready',
     'target-form', 'target-height', 'target-submit', 'target-cancel', 'target-state']
      .concat(['wifi-form', 'wifi-ssid', 'wifi-password', 'wifi-save', 'wifi-forget',
               'wifi-state', 'wifi-address', 'wifi-message', 'wifi-scan', 'wifi-scan-state',
               'wifi-networks', 'mqtt-form', 'mqtt-state', 'mqtt-message', 'mqtt-password-help',
               'mqtt-save', 'mqtt-enabled', 'mqtt-host', 'mqtt-port', 'mqtt-user', 'mqtt-password',
               'mqtt-clear-password', 'mqtt-prefix'])
      .map(id => [id, element()]));
  elements['control-ready'].textContent = 'Connecting controls...';
  const presets = [1, 2, 3].map(number => element({preset: String(number)}));
  const moves = ['up', 'down'].map(direction => element({direction}));
  const window = element();
  const document = element();
  document.getElementById = id => elements[id];
  document.createElement = () => element();
  document.querySelectorAll = selector => selector === '[data-preset]' ? presets : moves;
  const timers = new Map();
  let timerId = 0, commandId = 0;
  const client = {
    elements, presets, moves, window, document, timers, requests: [], responses: [],
    offline: false, scanGets: 0,
    scanResult: {state: 'complete', error: null, networks: []},
    status: {fresh: false, height_cm: null, last_known_height_cm: null,
      state: 'waiting_for_frames', panel_fresh: true, remote_ready: true,
      remote_button: 'idle', remote_command_id: 0, target_ready: false,
      target_active: false, target_command_id: 0, target_state: 'idle', target_reason: 'idle',
      wifi: {storage_ready: true, configured: false, ssid: '', connected: false,
        state: 'setup_ap', ap_active: true, ap_ssid: 'standing-desk-test', ap_ip: '192.168.4.1'},
      mqtt: {enabled:false,storage_ready:true,host:'',port:1883,username:'',password_saved:false,
        topic_prefix:'standing-desk/test',state:'disabled',error:null,command_error:null}}
  };
  const context = vm.createContext({
    document, window, AbortController, URLSearchParams,
    setTimeout(fn, delay) { timers.set(++timerId, {fn, delay}); return timerId; },
    clearTimeout(id) { timers.delete(id); },
    async fetch(url, options = {}) {
      if (url === '/api/status') {
        if (client.offline) throw new Error('offline');
        const snapshot = {...client.status};
        return {ok: true, json: async () => snapshot};
      }
      if(url === '/api/wifi/scan' && !options.method){
        ++client.scanGets;
        if(client.scanOffline)throw new Error('Scan connection lost');
        return {ok:true,json:async()=>client.scanResult};
      }
      const args = Object.fromEntries(options.body);
      client.requests.push({url, args, options});
      if (client.responses.length) return client.responses.shift()(url, args);
      if(url === '/api/mqtt')return {ok:true,json:async()=>({...client.status.mqtt,
        enabled:args.enabled==='true',host:args.host,port:Number(args.port),username:args.username,
        topic_prefix:args.topic_prefix,password_saved:args.password.length>0,state:'applying'})};
      if(url === '/api/wifi/scan')return {ok:true,
        json:async()=>({state:'running',error:null,networks:[]})};
      if(url === '/api/wifi' || url === '/api/wifi/forget')return {ok: true,
        json: async () => ({...client.status.wifi, configured: url === '/api/wifi',
          ssid: args.ssid || '', state: 'applying'})};
      const id = args.id ? Number(args.id) : ++commandId;
      return {ok: true, json: async () => ({command_id: id,
        target_command_id: id, target_active: url === '/api/target'})};
    }
  });
  client.run = code => vm.runInContext(code, context);
  client.poll = () => client.run('poll()');
  client.pollScan = () => client.run('pollWifiScan()');
  client.renew = async () => {
    const entry = [...timers].find(([, timer]) => timer.delay === 100);
    assert(entry, 'hold renewal should be scheduled');
    timers.delete(entry[0]);
    await entry[1].fn();
  };
  vm.runInContext(script, context);
  await flush();
  return client;
}

(async () => {
  // Readiness always follows firmware status, independently of action feedback.
  // A sleeping panel can allow manual controls while target movement is blocked.
  {
    const client = await page();
    assert.equal(client.elements['control-ready'].textContent, 'Controls ready');
    client.elements['control-state'].textContent = 'Previous request failed';
    client.status.remote_ready = false;
    await client.poll();
    assert.equal(client.elements['control-ready'].textContent, 'Controls busy or waiting for panel release');
    assert.equal(client.elements['control-state'].textContent, 'Previous request failed');
    Object.assign(client.status, {remote_button: 'up', remote_command_id: 12});
    await client.poll();
    assert.equal(client.elements['control-ready'].textContent, 'Remote command active: up');
    Object.assign(client.status, {remote_ready: true, remote_button: 'idle', panel_fresh: false,
      fresh: false, state: 'unsupported_display', last_known_height_cm: 70, target_ready: false});
    await client.poll();
    assert.equal(client.elements['control-ready'].textContent, 'Controls ready');
    assert(client.moves.every(button => !button.disabled));
    assert(client.presets.every(button => !button.disabled));
    assert(client.elements['target-submit'].disabled);
    client.offline = true;
    await client.poll();
    assert.equal(client.elements['control-ready'].textContent, 'Controls unavailable: monitor unreachable');
  }
  // Cached height persists across unsupported/stale displays but not ESP restart.
  {
    const client = await page();
    assert.equal(client.elements.height.textContent, 'No reading');
    for (const [state, fresh, last, shown] of [
      ['fresh', true, 78.6, '78.6 cm'],
      ['unsupported_display', false, 78.6, '78.6 cm'],
      ['stale', false, 78.6, '78.6 cm'],
      ['fresh', true, 105, '105 cm'],
      ['stale', false, 0, '0 cm'],
      ['waiting_for_frames', false, null, 'No reading']
    ]) {
      Object.assign(client.status, {state, fresh, last_known_height_cm: last,
        height_cm: fresh ? last : null});
      await client.poll();
      assert.equal(client.elements.height.textContent, shown);
      assert.equal(client.elements.state.textContent,
        (!fresh && last !== null ? 'Last known height. ' : '') + state.replaceAll('_', ' '));
    }
    client.offline = true;
    await client.poll();
    assert.equal(client.elements.state.textContent, 'Monitor unreachable');
    assert(client.presets.every(button => button.disabled));
    assert(client.moves.every(button => button.disabled));
  }

  // Each preset submits exactly once, with POST form fields and no retries.
  for (let number = 1; number <= 3; ++number) {
    const client = await page();
    await client.presets[number - 1].fire('click');
    assert.equal(client.requests.length, 1);
    const request = client.requests[0];
    assert.equal(request.url, '/api/preset');
    assert.deepEqual(request.args, {number: String(number)});
    assert.equal(request.options.method, 'POST');
    assert.equal(request.options.keepalive, true);
  }
  {
    const client = await page();
    client.responses.push(async () => { throw new Error('lost response'); });
    await client.presets[0].fire('click');
    assert.equal(client.requests.length, 1);
    assert.match(client.elements['control-state'].textContent, /lost response/);
  }

  // Pointer and keyboard holds renew only their original ID and release it.
  for (const direction of ['up', 'down']) {
    const client = await page();
    const button = client.moves.find(button => button.dataset.direction === direction);
    button.fire('pointerdown');
    await flush();
    assert.deepEqual(client.requests[0].args, {direction});
    await client.renew();
    assert.deepEqual(client.requests[1].args, {direction, id: '1'});
    button.fire('pointerup');
    await flush();
    assert.equal(client.requests[2].url, '/api/release');
    assert.deepEqual(client.requests[2].args, {id: '1'});
    assert(![...client.timers.values()].some(timer => timer.delay === 100));
  }
  {
    const client = await page();
    client.moves[0].fire('keydown', {key: 'Enter', repeat: false});
    client.moves[0].fire('keydown', {key: 'Enter', repeat: true});
    await flush();
    assert.equal(client.requests.length, 1);
    client.moves[0].fire('keyup', {key: 'Enter'});
    await flush();
    assert.equal(client.requests[1].url, '/api/release');
  }

  // Releasing before the start response arrives still releases the returned ID.
  {
    const client = await page();
    let respond;
    client.responses.push(() => new Promise(resolve => { respond = resolve; }));
    client.moves[0].fire('pointerdown');
    client.moves[0].fire('pointerup');
    assert.equal(client.requests.length, 1);
    respond({ok: true, json: async () => ({command_id: 42})});
    await flush();
    assert.equal(client.requests[1].url, '/api/release');
    assert.deepEqual(client.requests[1].args, {id: '42'});
    assert(![...client.timers.values()].some(timer => timer.delay === 100));
  }

  // A rejected renewal never retries with a fresh start request.
  {
    const client = await page();
    client.moves[0].fire('pointerdown');
    await flush();
    client.responses.push(async () => ({ok: false, status: 409,
      json: async () => ({error: 'Physical panel took priority'})}));
    await client.renew();
    await flush();
    assert.equal(client.requests.length, 3);
    assert.equal(client.requests[2].url, '/api/release');
    assert(![...client.timers.values()].some(timer => timer.delay === 100));
    assert.match(client.elements['control-state'].textContent, /Physical panel/);
  }

  // Polling also detects cancellation; physical action cannot restart the web hold.
  {
    const client = await page();
    client.moves[0].fire('pointerdown');
    await flush();
    Object.assign(client.status, {remote_button: 'idle', remote_command_id: 1,
      remote_ready: false});
    await client.poll();
    await flush();
    assert.equal(client.requests[1].url, '/api/release');
    assert(![...client.timers.values()].some(timer => timer.delay === 100));
  }

  // Cancel, capture loss, focus loss, page hide, and offline all release holds.
  for (const event of ['pointercancel', 'lostpointercapture', 'blur', 'pagehide',
                       'visibilitychange', 'offline']) {
    const client = await page();
    client.moves[0].fire('pointerdown');
    await flush();
    if (event === 'offline') { client.offline = true; await client.poll(); }
    else if (event === 'visibilitychange') {
      client.document.hidden = true;
      client.document.fire(event);
    } else if (event === 'blur' || event === 'pagehide') client.window.fire(event);
    else client.moves[0].fire(event);
    await flush();
    assert.equal(client.requests[1].url, '/api/release', event);
    assert(![...client.timers.values()].some(timer => timer.delay === 100), event);
  }

  // Cached heights never enable automatic controls. Fresh eligibility comes
  // from the firmware, and target commands are single POSTs without renewals.
  {
    const client = await page();
    client.elements['target-height'].value = '95';
    client.status.last_known_height_cm = 80;
    await client.poll();
    assert(client.elements['target-submit'].disabled);
    await client.elements['target-form'].fire('submit');
    assert.equal(client.requests.length, 0);
    client.status.target_ready = true;
    await client.poll();
    assert(!client.elements['target-submit'].disabled);
    await client.elements['target-form'].fire('submit');
    assert.equal(client.requests.length, 1);
    assert.equal(client.requests[0].url, '/api/target');
    assert.deepEqual(client.requests[0].args, {height_cm: '95'});
    assert(![...client.timers.values()].some(timer => timer.delay === 100));
    assert(client.presets.every(button => button.disabled));
    assert(client.moves.every(button => button.disabled));
    assert(!client.elements['target-cancel'].disabled);
    client.window.fire('pagehide');
    client.window.fire('blur');
    assert.equal(client.requests.length, 1, 'automatic movement is owned by the ESP');
    client.elements['target-cancel'].fire('click');
    await flush();
    assert.equal(client.requests[1].url, '/api/target/cancel');
    assert.deepEqual(client.requests[1].args, {id: '1'});
  }

  // An early cancel waits for the accepted target ID, then cancels that ID.
  {
    const client = await page();
    client.status.target_ready = true;
    await client.poll();
    client.elements['target-height'].value = '90';
    let respond;
    client.responses.push(() => new Promise(resolve => { respond = resolve; }));
    const pending = client.elements['target-form'].fire('submit');
    client.elements['target-cancel'].fire('click');
    respond({ok: true, json: async () => ({target_active: true, target_command_id: 42})});
    await pending;
    assert.equal(client.requests.length, 2);
    assert.equal(client.requests[1].url, '/api/target/cancel');
    assert.deepEqual(client.requests[1].args, {id: '42'});
  }

  // Refused starts are not retried, and completion/cancellation reasons are shown.
  {
    const client = await page();
    client.status.target_ready = true;
    await client.poll();
    client.elements['target-height'].value = '90';
    client.responses.push(async () => ({ok: false,
      json: async () => ({error: 'A fresh clean height display is required'})}));
    await client.elements['target-form'].fire('submit');
    assert.equal(client.requests.length, 1);
    assert.match(client.elements['control-state'].textContent, /fresh clean height/);
    Object.assign(client.status, {target_active: true, target_command_id: 5,
      target_height_cm: 90, target_direction: 'up', target_state: 'moving',
      target_reason: 'moving', target_ready: false, remote_ready: false});
    await client.poll();
    assert.equal(client.elements['target-state'].textContent, 'Moving up to 90 cm');
    Object.assign(client.status, {target_active: false, target_state: 'cancelled',
      target_reason: 'panel_override'});
    await client.poll();
    assert.equal(client.elements['target-state'].textContent, 'cancelled. panel override');
    assert(client.elements['target-cancel'].disabled);
  }
  // Wi-Fi setup saves form values, clears the entered password after success,
  // and never overwrites edits with a polling response.
  {
    const client = await page();
    assert(!client.elements['wifi-save'].disabled);
    client.elements['wifi-ssid'].value='My home';
    client.elements['wifi-ssid'].fire('input');
    client.elements['wifi-password'].value='test-password';
    await client.poll();
    assert.equal(client.elements['wifi-ssid'].value,'My home');
    await client.elements['wifi-form'].fire('submit');
    assert.equal(client.requests[0].url,'/api/wifi');
    assert.deepEqual(client.requests[0].args,{ssid:'My home',password:'test-password'});
    assert.equal(client.elements['wifi-password'].value,'');
    assert(!client.elements['wifi-forget'].disabled);
    await client.elements['wifi-forget'].fire('click');
    assert.equal(client.requests[1].url,'/api/wifi/forget');
    assert.equal(client.elements['wifi-ssid'].value,'');
    assert.match(client.elements['wifi-message'].textContent,/192\.168\.4\.1/);
  }
  // Invalid settings/storage errors keep input for correction and do not retry.
  {
    const client=await page();
    client.elements['wifi-ssid'].value='Home';
    client.elements['wifi-password'].value='short';
    client.responses.push(async()=>({ok:false,json:async()=>({error:'Invalid password'})}));
    await client.elements['wifi-form'].fire('submit');
    assert.equal(client.requests.length,1);
    assert.equal(client.elements['wifi-password'].value,'short');
    assert.match(client.elements['wifi-message'].textContent,/Invalid password/);
  }
  {
    const client=await page();
    Object.assign(client.status.wifi,{configured:true,ssid:'Home',station_ssid:'Home',
      state:'connected',connected:true,ip:'192.168.1.50'});
    await client.poll();
    assert.equal(client.elements['wifi-address'].href,'http://192.168.1.50/');
    assert.equal(client.elements['wifi-address'].hidden,false);
    assert.equal(client.elements['wifi-password'].value,'');
    Object.assign(client.status.wifi,{state:'fallback_ap',connected:false});
    await client.poll();
    assert.match(client.elements['wifi-state'].textContent,/Still trying/);
    assert.equal(client.elements['wifi-address'].hidden,true);
  }
  // Scanning is a single POST with GET polling. Selection copies literal SSID
  // text, never HTML, and does not save or overwrite credentials automatically.
  {
    const client=await page();
    client.elements['wifi-ssid'].value='Manual entry';
    client.elements['wifi-password'].value='my-password';
    await client.elements['wifi-scan'].fire('click');
    assert.equal(client.requests[0].url,'/api/wifi/scan');
    assert(client.elements['wifi-scan'].disabled);
    assert(client.elements['wifi-save'].disabled);
    await client.elements['wifi-scan'].fire('click');
    assert.equal(client.requests.length,1);
    client.scanResult={state:'running',error:null,networks:[]};
    await client.pollScan();
    assert(client.elements['wifi-scan'].disabled);
    client.scanResult={state:'complete',error:null,networks:[
      {ssid:'<b>Home</b>',rssi:-40,secured:true},{ssid:'Open',rssi:-70,secured:false}]};
    await client.pollScan();
    const choices=client.elements['wifi-networks'].children;
    assert.equal(choices.length,3);
    assert.equal(choices[1].textContent,'<b>Home</b> | -40 dBm | Password protected');
    assert.equal(choices[2].textContent,'Open | -70 dBm | Open');
    assert.equal(client.elements['wifi-ssid'].value,'Manual entry');
    client.elements['wifi-networks'].value='<b>Home</b>';
    client.elements['wifi-networks'].fire('change');
    await client.poll();
    assert.equal(client.elements['wifi-ssid'].value,'<b>Home</b>');
    assert.equal(client.elements['wifi-password'].value,'my-password');
    assert(!client.elements['wifi-networks'].disabled);
    assert(!client.elements['wifi-scan'].disabled);
    assert.equal(client.requests.length,1);
  }
  for(const failed of [false,true]){
    const client=await page();
    await client.elements['wifi-scan'].fire('click');
    client.scanResult={state:failed?'failed':'complete',error:failed?'Scan timed out':null,networks:[]};
    await client.pollScan();
    assert.match(client.elements['wifi-scan-state'].textContent,failed?/Scan timed out/:/No networks found/);
    assert(client.elements['wifi-networks'].disabled);
    assert(!client.elements['wifi-scan'].disabled);
    assert(!client.elements['wifi-ssid'].disabled);
  }
  {
    const client=await page();
    client.responses.push(async()=>({ok:false,json:async()=>({error:'Wi-Fi busy'})}));
    await client.elements['wifi-scan'].fire('click');
    assert.match(client.elements['wifi-scan-state'].textContent,/Wi-Fi busy/);
    assert(!client.elements['wifi-scan'].disabled);
    assert(!client.elements['wifi-save'].disabled);
  }
  {
    const client=await page();
    client.status.wifi.scan_state='running';
    await client.poll();
    assert(client.elements['wifi-scan'].disabled);
    assert.equal(client.requests.length,0); // Observe a scan started by another tab.
    await client.pollScan();
    assert.equal(client.scanGets,1);
    assert(!client.elements['wifi-scan'].disabled);
  }
  {
    const client=await page();
    await client.elements['wifi-scan'].fire('click');
    client.scanOffline=true;
    await client.pollScan();
    assert.match(client.elements['wifi-scan-state'].textContent,/Scan connection lost/);
    assert(!client.elements['wifi-scan'].disabled);
    assert(!client.elements['wifi-save'].disabled);
  }
  {
    const client=await page();
    assert.equal(client.elements['mqtt-port'].value,'1883');
    assert.equal(client.elements['mqtt-prefix'].value,'standing-desk/test');
    client.elements['mqtt-enabled'].checked=true;
    client.elements['mqtt-enabled'].fire('change');
    client.elements['mqtt-host'].value='192.168.1.50';
    client.elements['mqtt-user'].value='desk';
    client.elements['mqtt-password'].value='mqtt-test-password';
    client.elements['mqtt-prefix'].value='home/office/desk';
    await client.poll();
    assert.equal(client.elements['mqtt-host'].value,'192.168.1.50');
    await client.elements['mqtt-form'].fire('submit');
    assert.equal(client.requests[0].url,'/api/mqtt');
    assert.deepEqual(client.requests[0].args,{enabled:'true',host:'192.168.1.50',port:'1883',
      username:'desk',password:'mqtt-test-password',clear_password:'false',topic_prefix:'home/office/desk'});
    assert.equal(client.elements['mqtt-password'].value,'');
    assert.match(client.elements['mqtt-password-help'].textContent,/Leave blank to keep/);
    Object.assign(client.status.mqtt,{state:'retry_wait',error:'not_authorized'});
    await client.poll();
    assert.match(client.elements['mqtt-state'].textContent,/not authorized/);
  }
  {
    const client=await page();
    client.elements['mqtt-password'].value='keep-for-correction';
    client.responses.push(async()=>({ok:false,json:async()=>({error:'Storage failed'})}));
    await client.elements['mqtt-form'].fire('submit');
    assert.match(client.elements['mqtt-message'].textContent,/Storage failed/);
    assert.equal(client.elements['mqtt-password'].value,'keep-for-correction');
    assert(!client.elements['mqtt-save'].disabled);
    client.offline=true;
    await client.poll();
    assert(client.elements['mqtt-save'].disabled);
  }
  console.log('All web controls tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });

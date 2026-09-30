#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#include <Arduino.h>

// Single page UI. The settings table mirrors Widget::eepromFieldTable() in the
// Offline-Configurator, and the default EEPROM images mirror defaults.h.
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AM32 Configurator</title>
<style>
:root{
  --bg:#f4f5f7;--card:#fff;--text:#1b1f24;--muted:#5f6b7a;--line:#dde1e6;
  --accent:#2563eb;--accent-text:#fff;--ok:#15803d;--warn:#b45309;--bad:#b91c1c;
  --field:#f8f9fb;
}
@media (prefers-color-scheme:dark){
  :root{--bg:#111418;--card:#1a1e24;--text:#e6e9ee;--muted:#98a2b3;--line:#2c323b;
  --accent:#3b82f6;--accent-text:#fff;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--field:#14181d;}
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:center;justify-content:space-between;padding:12px 16px;background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:2}
header h1{font-size:16px;margin:0}
#status{font-weight:600}
#status.ok{color:var(--ok)}#status.warn{color:var(--warn)}#status.bad{color:var(--bad)}
main{max-width:1100px;margin:0 auto;padding:16px;display:grid;gap:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
.card h2{font-size:15px;margin:0 0 12px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button,.file{font:inherit;padding:8px 14px;border-radius:7px;border:1px solid var(--line);background:var(--field);color:var(--text);cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-text)}
button:disabled{opacity:.45;cursor:not-allowed}
.info{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin-top:12px}
.info div{background:var(--field);border:1px solid var(--line);border-radius:7px;padding:6px 10px}
.info span{display:block;color:var(--muted);font-size:12px}
.groups{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}
.group h3{font-size:13px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:0 0 8px}
.f{display:grid;grid-template-columns:1fr auto;gap:2px 10px;align-items:center;padding:6px 0;border-bottom:1px solid var(--line)}
.f:last-child{border-bottom:0}
.f label{min-width:0}
.f output{font-variant-numeric:tabular-nums;color:var(--muted);text-align:right;min-width:70px}
.f input[type=range]{grid-column:1/-1;width:100%}
.f select,.f input[type=number]{font:inherit;padding:4px 6px;border-radius:6px;border:1px solid var(--line);background:var(--field);color:var(--text);max-width:210px}
.f.check{grid-template-columns:auto 1fr}
.f.disabled{opacity:.5}
progress{width:100%;height:14px}
#log{background:var(--field);border:1px solid var(--line);border-radius:7px;padding:8px;height:140px;overflow:auto;font:12px/1.4 ui-monospace,Consolas,monospace;white-space:pre-wrap;margin:0}
.hint{color:var(--muted);font-size:12px;margin:8px 0 0}
.hidden{display:none!important}
input[type=file]{max-width:100%}
</style>
</head>
<body>
<header>
  <h1>AM32 ESC Configurator</h1>
  <div id="status">Not connected</div>
</header>
<main>
  <section class="card">
    <h2>ESC connection</h2>
    <div class="row">
      <button class="primary" id="btnConnect">Connect &amp; read settings</button>
      <button id="btnReset" disabled>Exit bootloader (run ESC)</button>
      <button id="btnOffline">Edit offline</button>
    </div>
    <p class="hint">With this device connected to the WiFi, power the ESC (signal wire and ground to the ESP32-C3). The held-high signal line keeps it in the bootloader. When nobody is on the WiFi, the receiver signal on GPIO 6 is passed through to the ESC instead. After "Exit bootloader", power-cycle the ESC to connect again.</p>
    <div class="info hidden" id="info">
      <div><span>Firmware</span><b id="iName">-</b></div>
      <div><span>Version</span><b id="iVer">-</b></div>
      <div><span>MCU / flash</span><b id="iMcu">-</b></div>
      <div><span>Bootloader protocol</span><b id="iProto">-</b></div>
    </div>
  </section>

  <section class="card hidden" id="settingsCard">
    <h2>Settings</h2>
    <div class="row" style="margin-bottom:12px">
      <button class="primary" id="btnWrite" disabled>Write settings to ESC</button>
      <button id="btnSave">Save config (.bin)</button>
      <label class="file">Load config (.bin)<input type="file" id="cfgFile" accept=".bin" class="hidden"></label>
    </div>
    <div class="groups" id="groups"></div>
  </section>

  <section class="card">
    <h2>Flash firmware</h2>
    <div class="row">
      <input type="file" id="fwFile" accept=".bin,.hex">
      <button class="primary" id="btnFlash" disabled>Flash firmware</button>
    </div>
    <div style="margin-top:12px"><progress id="progress" value="0" max="100"></progress></div>
    <div class="row" style="margin-top:12px">
      <button id="btnDefaults" disabled>Send default EEPROM</button>
      <button id="btnCrawler" disabled>Crawler defaults</button>
    </div>
  </section>

  <section class="card">
    <h2>Log</h2>
    <pre id="log"></pre>
  </section>
</main>
<script>
// defaults.h
const AIR=[0x01,0x04,0x01,0x01,0x23,0xA0,0x04,0x00,0x0A,0x64,0x00,0x32,0x02,0x00,0x35,0x31,0x20,
0x00,0x00,0x00,0x01,0x01,0x01,0x1A,0x18,0x64,0x37,0x0e,0x00,0x00,0x05,
0x00,0x80,0x80,0x80,0x32,0x00,0x32,0x00,0x00,0x0f,0x0a,0x0a,0x8d,0x66,0x06,0x00,0x00];
const CRAWLER=[0x01,0x02,0x01,0x01,0x23,0x4e,0x45,0x4f,0x45,0x53,0x43,0x20,0x66,0x30,0x35,0x31,0x20,
0x00,0x01,0x01,0x01,0x01,0x00,0x03,0x18,0x68,0x30,0x0e,0x01,0x01,0x05,
0x00,0x80,0x80,0x80,0x32,0x01,0x50,0x00,0x00,0x0f,0x0a,0x0a,0x65,0x14,0x05,0x00,0x31];

const GROUPS={motor:'Motor',throttle:'Throttle & brake',limits:'Protection',input:'Input & servo'};
const disabledText=(v,lim,txt)=>v>lim?'Disabled':txt;
// Widget::eepromFieldTable(): offset, control, range and the same display
// conversions the Qt slots use. legacy = forced value and gated = skipped
// when the ESC reports eeprom version 0.
const FIELDS=[
 {o:17,g:'motor',t:'check',l:'Reverse rotation'},
 {o:18,g:'motor',t:'check',l:'Bi-directional (fwd/rev)'},
 {o:19,g:'motor',t:'check',l:'Sinusoidal startup'},
 {o:20,g:'motor',t:'check',l:'Complementary PWM'},
 {o:47,g:'motor',t:'check',l:'Auto-timing'},
 {o:22,g:'motor',t:'check',l:'Stuck rotor protection'},
 {o:29,g:'motor',t:'check',l:'Stall protection'},
 {o:39,g:'motor',t:'check',l:'Use hall sensors',gated:1},
 {o:31,g:'motor',t:'check',l:'30 ms telemetry',legacy:0},
 {o:23,g:'motor',t:'range',l:'Timing advance',min:10,max:42,fmt:v=>((v-10)*0.9375).toFixed(2)+'°'},
 {o:25,g:'motor',t:'range',l:'Startup power',min:50,max:150,fmt:v=>v+' %'},
 {o:26,g:'motor',t:'range',l:'Motor KV',min:0,max:255,fmt:v=>(v*40+20)+' kv'},
 {o:27,g:'motor',t:'range',l:'Motor poles',min:2,max:36},
 {o:21,g:'motor',t:'select',l:'PWM frequency mode',opts:['Fixed','Variable PWM','Auto PWM frequency / by RPM']},
 {o:24,g:'motor',t:'range',l:'PWM frequency',min:8,max:144,fmt:v=>v+' kHz'},
 {o:40,g:'motor',t:'range',l:'Sine startup range',min:5,max:25,gated:1,fmt:v=>v+' %'},
 {o:45,g:'motor',t:'range',l:'Sine mode power',min:1,max:10,gated:1,toUi:v=>v>10?5:v},
 {o:30,g:'motor',t:'range',l:'Beep volume',min:0,max:11,legacy:5},

 {o:5,g:'throttle',t:'range',l:'Throttle rate of change',min:1,max:200,fmt:v=>(v/10).toFixed(1)+' %/ms'},
 {o:6,g:'throttle',t:'range',l:'Minimum duty cycle',min:0,max:50,fmt:v=>(v/2).toFixed(1)+' %'},
 {o:13,g:'throttle',t:'select',l:'Zero throttle behavior',opts:['Default','Coast on 0 throttle','Brake on 0 throttle','1 second delay then Brake on Stop','2 second delay then Brake on Stop','3 second delay then Brake on Stop','4 second delay then Brake on Stop','5 second delay then Brake on Stop']},
 {o:28,g:'throttle',t:'select',l:'Brake on stop',opts:['Off','Brake on 0 rpm, motor stopped','Active brake on stop']},
 {o:41,g:'throttle',t:'range',l:'Brake on stop level',min:1,max:10,gated:1},
 {o:12,g:'throttle',t:'range',l:'Active brake on stop power',min:0,max:10,fmt:v=>v+' %'},
 {o:42,g:'throttle',t:'range',l:'Running brake level',min:1,max:10,gated:1},
 {o:38,g:'throttle',t:'check',l:'Car / basher braking, double tap to reverse',gated:1},

 {o:36,g:'limits',t:'check',l:'Low voltage cut off (per cell)',gated:1},
 {o:37,g:'limits',t:'range',l:'Low voltage threshold',min:0,max:100,gated:1,fmt:v=>((v+250)/100).toFixed(2)+' V'},
 {o:8,g:'limits',t:'range',l:'Absolute voltage cut off',min:0,max:99},
 {o:43,g:'limits',t:'range',l:'Temperature limit',min:70,max:145,gated:1,toUi:v=>v<70?142:v,fmt:v=>disabledText(v,140,v+' °C')},
 {o:44,g:'limits',t:'range',l:'Current limit',min:1,max:102,gated:1,fmt:v=>disabledText(v,100,(v*2)+' A')},
 {o:9,g:'limits',t:'num',l:'Current limit P',min:0,max:500,scale:2},
 {o:10,g:'limits',t:'num',l:'Current limit I',min:0,max:255,scale:1},
 {o:11,g:'limits',t:'num',l:'Current limit D',min:0,max:500,scale:2},

 {o:46,g:'input',t:'select',l:'Signal type',gated:1,opts:['AUTO','Dshot 300 - 600','Servo 1000-2000us','Serial','Betaflight Safe Arming']},
 {o:7,g:'input',t:'check',l:'Disable stick calibration'},
 {o:32,g:'input',t:'range',l:'Servo low threshold',min:0,max:255,legacy:128,fmt:v=>(v*2+750)+' us'},
 {o:33,g:'input',t:'range',l:'Servo high threshold',min:0,max:255,legacy:128,fmt:v=>(v*2+1750)+' us'},
 {o:34,g:'input',t:'range',l:'Servo neutral',min:0,max:255,legacy:128,fmt:v=>(v+1374)+' us'},
 {o:35,g:'input',t:'range',l:'Servo dead band',min:1,max:100,legacy:50},
];

const $=id=>document.getElementById(id);
let esc=null;        // /api/connect result while the bootloader is connected
let eeprom=null;     // Widget::eeprom_buffer (48 bytes) or null
let busy=false;

function log(msg){const l=$('log');l.textContent+=new Date().toLocaleTimeString()+'  '+msg+'\n';l.scrollTop=l.scrollHeight;}
function status(msg,cls){const s=$('status');s.textContent=msg;s.className=cls||'';log(msg);}
const toHex=b=>Array.from(b,x=>x.toString(16).padStart(2,'0')).join('');
function fromHex(h){const o=new Uint8Array(h.length/2);for(let i=0;i<o.length;i++)o[i]=parseInt(h.substr(i*2,2),16);return o;}

async function api(path,body){
  const r=await fetch(path,{method:'POST',body:body===undefined?'':body,headers:{'Content-Type':'text/plain'}});
  if(!r.ok)throw new Error('HTTP '+r.status);
  return r.json();
}

// ---- settings form -------------------------------------------------------
function buildForm(){
  const groups={};
  for(const [k,name] of Object.entries(GROUPS)){
    const d=document.createElement('div');d.className='group';d.innerHTML='<h3>'+name+'</h3>';
    $('groups').appendChild(d);groups[k]=d;
  }
  for(const f of FIELDS){
    const row=document.createElement('div');row.className='f'+(f.t==='check'?' check':'');row.id='row'+f.o;
    const id='f'+f.o;
    if(f.t==='check'){
      row.innerHTML='<input type="checkbox" id="'+id+'"><label for="'+id+'">'+f.l+'</label>';
    }else if(f.t==='select'){
      row.innerHTML='<label for="'+id+'">'+f.l+'</label><select id="'+id+'">'+f.opts.map((o,i)=>'<option value="'+i+'">'+o+'</option>').join('')+'</select>';
    }else if(f.t==='num'){
      row.innerHTML='<label for="'+id+'">'+f.l+'</label><input type="number" id="'+id+'" min="'+f.min+'" max="'+f.max+'" step="'+f.scale+'">';
    }else{
      row.innerHTML='<label for="'+id+'">'+f.l+'</label><output id="o'+f.o+'"></output><input type="range" id="'+id+'" min="'+f.min+'" max="'+f.max+'">';
      row.querySelector('input').addEventListener('input',()=>showValue(f));
    }
    groups[f.g].appendChild(row);
    $(id).addEventListener('change',updateDependencies);
  }
}
function showValue(f){const v=+$('f'+f.o).value;$('o'+f.o).textContent=f.fmt?f.fmt(v):String(v);}
function setField(f,v){
  const el=$('f'+f.o);
  if(f.toUi)v=f.toUi(v);
  if(f.t==='check')el.checked=v===1;
  else if(f.t==='num')el.value=v*f.scale;
  else if(f.t==='select')el.value=v<f.opts.length?v:0;
  else{el.value=Math.min(f.max,Math.max(f.min,v));showValue(f);}
}
function getField(f){
  const el=$('f'+f.o);
  if(f.t==='check')return el.checked?1:0;
  if(f.t==='num'){const v=Math.min(f.max,Math.max(f.min,parseInt(el.value,10)||0));return Math.floor(v/f.scale)&0xFF;}
  return (+el.value)&0xFF;
}
function setEnabled(o,on){$('f'+o).disabled=!on;$('row'+o).classList.toggle('disabled',!on);}
function updateDependencies(){
  // on_varPWMCheckBox_stateChanged, on_brakecheckbox_/on_activeBrakeCheckbox_stateChanged
  setEnabled(24,+$('f21').value!==1);
  setEnabled(41,+$('f28').value===1);
  setEnabled(12,+$('f28').value===2);
}
// Widget::applyBufferToUi()
function applyBufferToUi(buf){
  const ver=buf[1];
  for(const f of FIELDS){
    if(f.gated&&ver===0)continue;
    setField(f,(f.legacy!==undefined&&ver===0)?f.legacy:buf[f.o]);
  }
  updateDependencies();
}
// Widget::buildBufferFromUi()
function buildBufferFromUi(base){
  const out=Uint8Array.from(base.slice(0,48));
  for(const f of FIELDS)out[f.o]=getField(f);
  return out;
}

// ---- UI state ------------------------------------------------------------
function refresh(){
  const online=!!esc;
  $('btnConnect').disabled=busy;
  $('btnOffline').disabled=busy;
  $('btnReset').disabled=busy||!online;
  $('btnWrite').disabled=busy||!online||!eeprom;
  $('btnFlash').disabled=busy||!online;
  $('btnDefaults').disabled=busy||!online;
  $('btnCrawler').disabled=busy||!online;
}
async function run(fn){
  if(busy)return;
  busy=true;refresh();
  try{await fn();}catch(e){status('Error: '+e.message,'bad');}
  busy=false;refresh();
}
function showSettings(on){$('settingsCard').classList.toggle('hidden',!on);}
const MCU={0x1f:'32k (F0 / 1k pages)',0x35:'64k (2k pages)',0x2b:'128k (G071 / 2k pages)',0x15:'NXP 64k (8k pages)'};

// ---- ESC operations ------------------------------------------------------
async function connectEsc(){
  status('Connecting to ESC...');
  let r=await api('/api/connect');
  if(!r.ok){log('Did not connect ('+r.error+') - retrying');r=await api('/api/connect');}
  if(!r.ok){esc=null;status('Can not connect: '+r.error,'bad');return;}
  esc=r;
  const b=fromHex(r.eeprom);
  $('info').classList.remove('hidden');
  $('iName').textContent=r.name||'-';
  $('iMcu').textContent=MCU[r.flashCode]||('0x'+r.flashCode.toString(16));
  $('iProto').textContent=r.protocol;
  $('iVer').textContent='-';
  // the checks at the end of Widget::connectMotor()
  if(b[0]===0xFF){eeprom=null;showSettings(false);status('Connected - boot bit set to 0xFF','warn');return;}
  if(b[0]!==0x01){
    eeprom=new Uint8Array(48);showSettings(false);
    status('Connected - No firmware found, please flash the latest AM32 firmware','warn');return;
  }
  if(b[1]===0xFF||b[2]===0x00){
    eeprom=null;showSettings(false);
    status("Connected - No EEprom. Use 'Send default EEPROM'",'warn');return;
  }
  if(b[1]<0x03){
    eeprom=null;showSettings(false);
    status('Connected - Firmware update required (this tool is for 2.19 or higher)','warn');return;
  }
  applyBufferToUi(b);
  eeprom=b;
  $('iVer').textContent=b[3]+'.'+b[4];
  showSettings(true);
  status('Connected: settings read OK','ok');
}

async function writeEeprom(buf){
  const r=await api('/api/eeprom',toHex(buf));
  if(!r.ok)log('EEPROM write failed: '+r.error);
  return r.ok;
}

async function writeSettings(){
  const out=buildBufferFromUi(eeprom);
  if(await writeEeprom(out)){eeprom=out;status('WRITE EEPROM SUCCESSFUL','ok');}
  else status('EEPROM write failed','bad');
}

async function resetEsc(){
  await api('/api/reset');
  esc=null;
  status('ESC running firmware. Power-cycle it to connect again.');
}

// Widget::sendFirstEeprom()
async function sendFirstEeprom(defaults){
  if(await writeEeprom(Uint8Array.from(defaults)))status('WRITE DEFAULT SUCCESS','ok');
  else status('Default EEPROM write failed','bad');
  showSettings(false);
}

// ---- firmware files ------------------------------------------------------
function parseIntelHex(text){
  const data=new Map();let base=0,min=Infinity,max=-1;
  for(const raw of text.split(/\r?\n/)){
    const line=raw.trim();
    if(!line)continue;
    if(line[0]!==':'||line.length%2!==1)throw new Error('Not an Intel HEX file');
    const b=fromHex(line.slice(1));
    if(b.reduce((s,x)=>s+x,0)&0xFF)throw new Error('CRC ERROR IN HEX FILE!');
    const n=b[0],addr=(b[1]<<8)|b[2],type=b[3];
    if(type===0){
      const a=base+addr;
      for(let i=0;i<n;i++)data.set(a+i,b[4+i]);
      min=Math.min(min,a);max=Math.max(max,a+n);
    }else if(type===1)break;
    else if(type===2)base=((b[4]<<8)|b[5])*16;
    else if(type===4)base=((b[4]<<8)|b[5])*65536;
  }
  if(max<0)throw new Error('HEX file has no data');
  const image=new Uint8Array(max-min);  // gaps are zero filled like convertFromHex()
  for(const [a,v] of data)image[a-min]=v;
  return {image,start:min};
}

async function loadFirmware(file){
  const ext=file.name.split('.').pop().toLowerCase();
  if(ext==='bin')return new Uint8Array(await file.arrayBuffer());
  if(ext!=='hex')throw new Error('NOT A VALID FILE - select a .bin or .hex file');
  const {image,start}=parseIntelHex(await file.text());
  const rel=start>=0x08000000?start-0x08000000:start;
  if(rel!==esc.firmwareStart&&!confirm('HEX data starts at 0x'+start.toString(16)+', but this ESC expects firmware at offset 0x'+esc.firmwareStart.toString(16)+'. Flash anyway?'))
    throw new Error('Flash cancelled');
  return image;
}

// Widget::on_writeBinary_clicked() in direct mode
async function flashFirmware(){
  const file=$('fwFile').files[0];
  if(!file){status('Select a .bin or .hex file','warn');return;}
  const image=await loadFirmware(file);
  if(image.length>esc.firmwareAreaSize)throw new Error('Firmware ('+image.length+' bytes) is larger than the '+esc.firmwareAreaSize+' byte application area');

  if(eeprom){  // clear the boot byte so a partial flash never gets started
    const out=Uint8Array.from(eeprom);out[0]=0x00;
    if(!await writeEeprom(out)){status('Unable to set safety bit','bad');return;}
    log('WRITE EEPROM SUCCESSFUL (safety bit cleared)');
  }

  const CHUNK=128,MAX_RETRIES=8;
  $('progress').value=0;
  for(let off=0;off<image.length;off+=CHUNK){
    let chunk=image.slice(off,off+CHUNK);
    if(chunk.length%8){const p=new Uint8Array(Math.ceil(chunk.length/8)*8).fill(0xFF);p.set(chunk);chunk=p;}
    let r;
    for(let attempt=0;attempt<=MAX_RETRIES;attempt++){
      r=await api('/api/flash?offset='+off,toHex(chunk));
      if(r.ok)break;
      log('Chunk at '+off+' failed ('+r.error+'), retrying');
    }
    if(!r.ok){status('FLASH FAILURE at offset '+off,'bad');return;}
    $('progress').value=Math.round(Math.min(off+CHUNK,image.length)*100/image.length);
  }
  status('FLASH SUCCESS','ok');
  $('progress').value=0;

  if(!eeprom)return;
  if(eeprom[1]<0x03||eeprom[2]===0x00){  // no settings yet: send defaults and start
    await sendFirstEeprom(AIR);
    await resetEsc();
    return;
  }
  const out=Uint8Array.from(eeprom);out[0]=0x01;
  if(await writeEeprom(out))status('FLASH SUCCESS - WRITE EEPROM SUCCESSFUL','ok');
  else status('Unable to set safety bit','bad');
}

// ---- config files (offline mode) ------------------------------------------
function saveConfig(){
  const out=buildBufferFromUi(eeprom||AIR);
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([out],{type:'application/octet-stream'}));
  a.download='am32_v3_config.bin';a.click();
  setTimeout(()=>URL.revokeObjectURL(a.href),1000);
  log('Config file saved');
}
// Widget::loadConfig()
async function loadConfig(file){
  const buf=new Uint8Array(await file.arrayBuffer());
  if(buf.length<48){status('Config file too short','bad');return;}
  if(buf[0]!==0x01){status('Not an AM32 config file (boot byte is not 1)','bad');return;}
  if(buf[1]===0xFF||buf[2]===0x00)log('Warning: config file has no settings');
  if(buf[1]<0x01)log('Warning: config file is from outdated firmware');
  applyBufferToUi(buf);
  eeprom=buf.slice(0,48);
  showSettings(true);
  status('Config file loaded: '+file.name,'ok');
}
function editOffline(){
  applyBufferToUi(AIR);
  eeprom=Uint8Array.from(AIR);
  showSettings(true);
  status('Offline mode: editing default settings');
}

// ---- wiring --------------------------------------------------------------
buildForm();
$('btnConnect').onclick=()=>run(connectEsc);
$('btnReset').onclick=()=>run(resetEsc);
$('btnOffline').onclick=editOffline;
$('btnWrite').onclick=()=>run(writeSettings);
$('btnSave').onclick=saveConfig;
$('cfgFile').onchange=e=>{const f=e.target.files[0];e.target.value='';if(f)run(()=>loadConfig(f));};
$('btnFlash').onclick=()=>run(flashFirmware);
$('btnDefaults').onclick=()=>run(()=>sendFirstEeprom(AIR));
$('btnCrawler').onclick=()=>run(async()=>{await sendFirstEeprom(CRAWLER);await resetEsc();});
refresh();
</script>
</body>
</html>
)rawliteral";

#endif  // WEB_PAGE_H

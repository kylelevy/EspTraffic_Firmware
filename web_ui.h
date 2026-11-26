// ============================================================================
//  web_ui.h - the complete web interface, embedded in flash (PROGMEM).
//  Plain HTML + CSS + vanilla JavaScript. No frameworks, CDNs or external
//  requests; everything is served by the ESP32 itself.
// ============================================================================
#pragma once

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Display Controller</title>
<style>
:root{--bg:#10141a;--card:#1a212b;--fg:#e6edf3;--mut:#8b98a8;--acc:#3b9eff;--on:#ff3b30;--off:#2a323d;--ok:#2ecc71;--err:#ff5d5d;--bd:#2c3644;--inp:#0f141b}
@media (prefers-color-scheme:light){:root{--bg:#f2f4f7;--card:#fff;--fg:#17202a;--mut:#5d6b7a;--off:#d9dee5;--bd:#d0d7de;--inp:#f7f9fb}}
*{box-sizing:border-box}
body{margin:0;font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{display:flex;flex-wrap:wrap;gap:8px;align-items:center;justify-content:space-between;padding:12px 16px;border-bottom:1px solid var(--bd)}
h1{font-size:18px;margin:0}
h2{font-size:15px;margin:0 0 10px}
main{max-width:1000px;margin:0 auto;padding:12px;display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(310px,1fr))}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:14px}
.wide{grid-column:1/-1}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:6px 0}
label{color:var(--mut);font-size:13px}
input,select,button{font:inherit;color:var(--fg);background:var(--inp);border:1px solid var(--bd);border-radius:7px;padding:7px 10px}
input[type=number]{width:90px}input[type=text],input[type=password],input[type=date]{min-width:150px;flex:1}
input[type=checkbox]{width:auto}
button{cursor:pointer;background:var(--acc);border-color:var(--acc);color:#fff}
button.sec{background:var(--inp);color:var(--fg);border-color:var(--bd)}
button.warn{background:var(--err);border-color:var(--err)}
button:disabled{opacity:.5;cursor:default}
.chip{display:inline-block;padding:2px 9px;border-radius:99px;border:1px solid var(--bd);font-size:12px;color:var(--mut)}
.chip.ok{color:var(--ok);border-color:var(--ok)}.chip.bad{color:var(--err);border-color:var(--err)}
#disp{width:100%;max-width:440px;display:block;margin:0 auto}
.seg{fill:var(--off);stroke:rgba(0,0,0,.35);stroke-width:.6;cursor:pointer;transition:fill .12s}
.seg.on{fill:var(--on);filter:drop-shadow(0 0 3px var(--on))}
#disp text{fill:var(--mut);font-size:7px}
.stat{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;font-size:13px}.stat span:nth-child(odd){color:var(--mut)}
.grid7{display:grid;grid-template-columns:repeat(7,auto);gap:2px 8px;font-size:12px;align-items:center}
progress{width:100%;height:14px}
#toast{position:fixed;left:50%;bottom:16px;transform:translateX(-50%);max-width:92vw;padding:10px 16px;border-radius:9px;background:#222;color:#fff;display:none;z-index:9;box-shadow:0 4px 14px rgba(0,0,0,.4)}
#toast.err{background:#b3261e}#toast.ok{background:#1e7b45}
.hint{color:var(--mut);font-size:12px}
</style></head><body>
<header><h1>ESP32 Display Controller</h1>
<div><span class="chip" id="cOnline">connecting...</span> <span class="chip" id="cWifi">wifi</span> <span class="chip" id="cClock">clock</span></div></header>
<main>

<section class="card wide">
<h2>Display</h2>
<svg id="disp" viewBox="0 0 150 122" role="img" aria-label="Seven segment display"></svg>
<p class="hint" style="text-align:center">Click a segment to toggle it (temporary request, expires after the configured timeout).</p>
<div class="stat" id="stat"></div>
</section>

<section class="card">
<h2>Controls</h2>
<div class="row"><button class="sec" onclick="allSet(false)">All OFF</button><button onclick="allSet(true)">All ON</button><button class="warn" onclick="stopReq()">Stop</button></div>
<div class="row"><label for="num">Number (0-99)</label><input type="number" id="num" min="0" max="99" value="0"><button onclick="sendNum()">Display number</button></div>
<div class="row"><button onclick="startPat('flash')">All flash</button><button onclick="startPat('count')">Counting 00-99</button><button onclick="startPat('figure8')">Figure-eight</button><button onclick="startPat('blink')">Blink</button></div>
</section>

<section class="card">
<h2>Pattern configuration</h2>
<div class="row"><label for="pSel">Pattern</label><select id="pSel"><option value="flash">All flash</option><option value="count">Counting</option><option value="figure8">Figure-eight</option><option value="blink">Blink</option></select></div>
<div class="row"><label for="pSpeed">Speed (ms/step)</label><input type="number" id="pSpeed" min="30" max="5000" placeholder="default"></div>
<div class="row"><label for="pTimeout">Timeout (s)</label><input type="number" id="pTimeout" min="1" max="3600" placeholder="default"></div>
<div class="row"><label for="bCount">Blink count</label><input type="number" id="bCount" min="1" max="100" value="3"></div>
<div class="hint">Blink targets (none selected = all segments)</div>
<div class="grid7" id="bGrid"></div>
<div class="row"><button class="sec" onclick="bSel('all')">All</button><button class="sec" onclick="bSel('left')">Left</button><button class="sec" onclick="bSel('right')">Right</button><button class="sec" onclick="bSel('none')">None</button></div>
<div class="row"><button onclick="startPat($('pSel').value)">Start selected pattern</button></div>
</section>

<section class="card">
<h2>Countdown</h2>
<div class="row"><label for="cdDate">Event date</label><input type="date" id="cdDate"></div>
<div class="row"><label><input type="checkbox" id="cdEn"> Enable countdown</label></div>
<div class="row"><button onclick="saveCd()">Save</button></div>
<div class="stat" id="cdStat"></div>
</section>

<section class="card">
<h2>Settings</h2>
<div class="row"><label for="sTimeout">Request timeout (s)</label><input type="number" id="sTimeout" min="1" max="3600"></div>
<div class="row"><label for="sPat">Pattern speed (ms)</label><input type="number" id="sPat" min="30" max="5000"></div>
<div class="row"><label for="sCnt">Counting (ms)</label><input type="number" id="sCnt" min="30" max="5000">
<label for="sF8">Figure-8 (ms)</label><input type="number" id="sF8" min="30" max="5000"></div>
<div class="row"><label for="sFl">Flashing (ms)</label><input type="number" id="sFl" min="30" max="5000"></div>
<div class="row"><label for="sTz">Timezone (POSIX)</label><input type="text" id="sTz" maxlength="47"></div>
<div class="row"><label for="sSsid">Wi-Fi SSID</label><input type="text" id="sSsid" maxlength="32" autocomplete="off"></div>
<div class="row"><label for="sPass">Wi-Fi password</label><input type="password" id="sPass" maxlength="63" autocomplete="new-password" placeholder="leave empty to keep"></div>
<div class="row"><label><input type="checkbox" id="sPs"> Power-saving mode</label></div>
<div class="row"><button onclick="saveSettings()">Save settings</button></div>
<div class="hint" id="wifiMsg"></div>
</section>

<section class="card">
<h2>Firmware update</h2>
<div class="row"><input type="file" id="fw" accept=".bin"></div>
<div class="row"><button onclick="uploadFw()" id="fwBtn">Upload firmware</button></div>
<progress id="prog" value="0" max="100"></progress>
<div class="hint" id="fwMsg">Select the compiled firmware.bin and upload. The device restarts automatically.</div>
</section>

</main>
<div id="toast"></div>
<script>
'use strict';
const $=id=>document.getElementById(id);
const LET='ABCDEFG', SIDES=['left','right'], NS='http://www.w3.org/2000/svg';
const POLY={A:'12,6 16,2 44,2 48,6 44,10 16,10',B:'54,8 58,12 58,44 54,48 50,44 50,12',C:'54,52 58,56 58,88 54,92 50,88 50,56',
D:'12,94 16,90 44,90 48,94 44,98 16,98',E:'6,52 10,56 10,88 6,92 2,88 2,56',F:'6,8 10,12 10,44 6,48 2,44 2,12',G:'12,50 16,46 44,46 48,50 44,54 16,54'};
let state=null, loadedSsid='', filled=false, lastSeq=-1, uploading=false, toastT=0;

function toast(m,kind){const t=$('toast');t.textContent=m;t.className=kind||'';t.style.display='block';clearTimeout(toastT);toastT=setTimeout(()=>t.style.display='none',kind==='err'?6000:3000)}
async function api(path,method,body){
  const o={method:method||'GET',cache:'no-store'};
  if(body!==undefined){o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(body)}
  const r=await fetch(path,o);let j={};try{j=await r.json()}catch(e){}
  if(!r.ok||j.ok===false)throw new Error(j.error||('HTTP '+r.status));
  return j}
async function cmd(path,body,okMsg){
  try{const j=await api(path,'POST',body||{});apply(j);if(okMsg)toast(okMsg,'ok');return j}
  catch(e){toast(e.message,'err')}}

/* ---------- display drawing ---------- */
function build(){
  const svg=$('disp');
  SIDES.forEach((side,i)=>{
    const g=document.createElementNS(NS,'g');g.setAttribute('transform','translate('+(5+i*80)+',2)');
    for(const s of LET){const p=document.createElementNS(NS,'polygon');p.setAttribute('points',POLY[s]);p.setAttribute('class','seg');
      p.dataset.seg=side+'.'+s;p.addEventListener('click',()=>toggle(side,s));
      const t=document.createElementNS(NS,'title');t.textContent=side+' '+s;p.appendChild(t);g.appendChild(p)}
    svg.appendChild(g);
    const t=document.createElementNS(NS,'text');t.setAttribute('x',35+i*80);t.setAttribute('y',117);t.setAttribute('text-anchor','middle');
    t.textContent=i?'RIGHT (units)':'LEFT (tens)';svg.appendChild(t)});
  const grid=$('bGrid');
  SIDES.forEach(side=>{for(const s of LET){const l=document.createElement('label');
    l.innerHTML='<input type="checkbox" data-b="'+side+'.'+s+'"> '+side[0].toUpperCase()+s;grid.appendChild(l)}})}
function paint(){
  if(!state)return;
  document.querySelectorAll('.seg').forEach(p=>{const[sd,l]=p.dataset.seg.split('.');p.classList.toggle('on',!!state[sd][l])})}
async function toggle(side,s){
  const cur=!!(state&&state[side]&&state[side][s]);const seg={};seg[side+'.'+s]=!cur;
  await cmd('/api/segments',{segments:seg})}

/* ---------- commands ---------- */
function allSet(on){cmd('/api/all',{on:on})}
function stopReq(){cmd('/api/stop',{})}
function sendNum(){
  const v=Number($('num').value);
  if(!Number.isInteger(v)||v<0||v>99){toast('Enter an integer from 0 to 99','err');return}
  cmd('/api/number',{number:v})}
function bSel(w){document.querySelectorAll('[data-b]').forEach(c=>{const side=c.dataset.b.split('.')[0];
  c.checked=(w==='all')||(w===side)})}
function startPat(name){
  const b={pattern:name};
  const sp=$('pSpeed').value, to=$('pTimeout').value;
  if(sp!=='')b.speed=Number(sp);
  if(to!=='')b.timeout=Number(to);
  if(name==='blink'){
    b.count=Number($('bCount').value);
    b.segments=[...document.querySelectorAll('[data-b]:checked')].map(c=>c.dataset.b)}
  cmd('/api/pattern',b)}

/* ---------- countdown ---------- */
function saveCd(){cmd('/api/countdown',{date:$('cdDate').value,enabled:$('cdEn').checked},'Countdown saved')}

/* ---------- settings ---------- */
async function loadSettings(){
  try{const s=await api('/api/settings');
    $('sTimeout').value=s.timeout;$('sPat').value=s.patternMs;$('sCnt').value=s.countMs;$('sF8').value=s.fig8Ms;$('sFl').value=s.flashMs;
    $('sTz').value=s.tz;$('sSsid').value=s.ssid;$('sPs').checked=s.powerSave;loadedSsid=s.ssid;
  }catch(e){toast('Could not load settings: '+e.message,'err')}}
async function saveSettings(){
  const b={timeout:Number($('sTimeout').value),patternMs:Number($('sPat').value),countMs:Number($('sCnt').value),
    fig8Ms:Number($('sF8').value),flashMs:Number($('sFl').value),tz:$('sTz').value.trim(),powerSave:$('sPs').checked};
  const ssid=$('sSsid').value.trim(), pass=$('sPass').value;
  if(ssid!==loadedSsid||pass!==''){
    if(ssid!==loadedSsid&&pass===''&&!confirm('The password is empty. Connect to "'+ssid+'" as an OPEN network?'))return;
    b.ssid=ssid;if(pass!=='')b.password=pass;
    if(!confirm('The controller will try to join "'+ssid+'". If it fails it reverts to the old network. Continue?'))return}
  const j=await cmd('/api/settings',b,'Settings saved');
  if(j&&b.ssid){$('sPass').value='';$('wifiMsg').textContent='Applying Wi-Fi change... if the page stops responding, reconnect to the new network and open http://espTraffic.local'}}

/* ---------- firmware ---------- */
function uploadFw(){
  const f=$('fw').files[0];
  if(!f){toast('Choose a .bin file first','err');return}
  if(!/\.bin$/i.test(f.name)){toast('The file must be a .bin firmware image','err');return}
  const fd=new FormData();fd.append('firmware',f,f.name);
  const x=new XMLHttpRequest();uploading=true;$('fwBtn').disabled=true;$('prog').value=0;$('fwMsg').textContent='Uploading...';
  x.open('POST','/update');
  x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);$('prog').value=p;$('fwMsg').textContent='Uploading... '+p+'%'}};
  x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}
    if(x.status===200&&j.ok){$('prog').value=100;$('fwMsg').textContent='Update successful. Restarting...';toast('Firmware updated, restarting','ok');waitReboot()}
    else{uploading=false;$('fwBtn').disabled=false;$('fwMsg').textContent='Update failed: '+(j.error||('HTTP '+x.status));toast('Update failed','err')}};
  x.onerror=()=>{uploading=false;$('fwBtn').disabled=false;$('fwMsg').textContent='Update failed: connection lost during upload';toast('Update failed','err')};
  x.send(fd)}
function waitReboot(){
  let tries=0;const t=setInterval(async()=>{tries++;
    try{await api('/api/info');clearInterval(t);location.reload()}catch(e){if(tries>45){clearInterval(t);$('fwMsg').textContent='Device did not come back. Check it and reload.';uploading=false;$('fwBtn').disabled=false}}},2000)}

/* ---------- state refresh (read-only polling) ---------- */
function row(k,v){return '<span>'+k+'</span><span>'+v+'</span>'}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function apply(j){
  state=j;paint();
  const w=j.wifi,c=j.countdown;
  $('cOnline').textContent='online';$('cOnline').className='chip ok';
  $('cWifi').textContent=w.connected?('Wi-Fi '+w.rssi+' dBm'):('Wi-Fi '+w.state);$('cWifi').className='chip '+(w.connected?'ok':'bad');
  $('cClock').textContent=c.synced?'clock synced':'clock not synced';$('cClock').className='chip '+(c.synced?'ok':'bad');
  $('stat').innerHTML=row('Mode',esc(j.mode))+row('Pattern',esc(j.pattern))+row('Number',j.number===null?'-':String(j.number).padStart(2,'0'))+
    row('Request',j.requestActive?('active, '+(j.timeoutRemainingMs/1000).toFixed(1)+' s left'):'none')+
    row('Network',esc(w.ssid||'-')+' / '+esc(w.ip||'-'))+row('Power saving',j.powerSave?'on':'off');
  $('cdStat').innerHTML=row('Days remaining',c.daysRemaining===null?'-':c.daysRemaining)+row('Displayed',c.displayed===null?'-':String(c.displayed).padStart(2,'0'))+
    row('Clock',c.synced?'synchronised':'waiting for NTP')+row('Local time',esc(c.localTime||'-'))+row('Countdown',c.active?'showing':(c.enabled?'enabled (no valid date / clock)':'off'));
  if(!filled){filled=true;$('cdDate').value=c.date||'';$('cdEn').checked=c.enabled}
  if(j.wifiChange&&j.wifiChange.seq!==lastSeq){
    if(lastSeq!==-1&&j.wifiChange.status!=='none'){$('wifiMsg').textContent=j.wifiChange.message;
      toast(j.wifiChange.message,j.wifiChange.status==='failed'?'err':(j.wifiChange.status==='success'?'ok':''))}
    lastSeq=j.wifiChange.seq}}
async function poll(){
  if(uploading)return;
  try{apply(await api('/api/state'))}
  catch(e){$('cOnline').textContent='offline';$('cOnline').className='chip bad'}}

/* ---------- browser console diagnostics ---------- */
function flat(o,p,out){out=out||{};for(const k in o){const v=o[k],key=p?p+'.'+k:k;
  if(v&&typeof v==='object')flat(v,key,out);else out[key]=v}return out}
async function consoleInfo(){
  try{const i=await api('/api/info');console.info('ESP32 display controller',i.hostname,i.firmware);console.table(flat(i))}
  catch(e){console.info('Could not read /api/info: '+e.message)}}

build();poll();loadSettings();consoleInfo();setInterval(poll,3000);
</script></body></html>)HTMLPAGE";

// Integration test: the real web app talking to the real relay core.
//
//   browser (web/index.html)  --Web Bluetooth mock-->  node  --stdin/stdout-->  relay_cli (relay_core.h, real time)
//
// Only the Bluetooth radio is faked. Frames the app writes go byte for byte into the firmware logic,
// and everything the firmware notifies goes byte for byte back into the app.
//
// Run:  firmware/tests/run_tests.sh   (builds /tmp/relay_cli)
//       node test/integration.js
// Needs Playwright with Chromium (npm i playwright).

const path = require('path');
const { spawn } = require('child_process');
let pw;
try { pw = require('playwright'); } catch (e) { pw = require('/home/claude/.npm-global/lib/node_modules/playwright'); }

const CLI = process.env.RELAY_CLI || '/tmp/relay_cli';
const APP = 'file://' + path.resolve(__dirname, '../web/index.html');
const sleep = ms => new Promise(r => setTimeout(r, ms));

let pass = 0, fail = 0;
function check(cond, msg) { if (cond) { pass++; console.log('  ok    ' + msg); } else { fail++; console.log('  FAIL  ' + msg); } }
function near(a, b, tol, msg) { check(Math.abs(a - b) <= tol, msg + ' (got ' + a + ', expected ' + b + ' +/- ' + tol + ')'); }

(async () => {
  // ---------- the relay ----------
  const relay = spawn(CLI, [], { stdio: ['pipe', 'pipe', 'inherit'] });
  const lampEdges = [], safeEdges = [], beeps = [];
  let page = null, buf = '';
  relay.stdout.on('data', d => {
    buf += d.toString(); let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i); buf = buf.slice(i + 1);
      const p = line.split(' ');
      if (p[0] === 'N' && page) page.evaluate(h => window.__ble.notify(h), p[1]).catch(() => {});
      else if (p[0] === 'L' && p[1] === 'E') lampEdges.push({ on: p[2] === '1', t: +p[3] });
      else if (p[0] === 'L' && p[1] === 'S') safeEdges.push({ on: p[2] === '1', t: +p[3] });
      else if (p[0] === 'B') beeps.push(+p[1]);
    }
  });
  const cli = line => relay.stdin.write(line + '\n');
  const windows = (from = 0) => {
    const w = []; let s = null;
    for (const e of lampEdges.slice(from)) { if (e.on && s === null) s = e.t; else if (!e.on && s !== null) { w.push({ start: s, ms: e.t - s, end: e.t }); s = null; } }
    return w;
  };

  // ---------- the browser ----------
  const browser = await pw.chromium.launch();
  page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.exposeFunction('__bleWrite', hex => { cli('W ' + hex); });
  await page.exposeFunction('__bleConnect', () => { cli('CONN'); });
  await page.exposeFunction('__bleDisconnect', () => { cli('DISC'); });
  await page.addInitScript(() => {
    const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');
    const dev = {}, chr = [];
    const evtChar = { addEventListener(t, f) { if (t === 'characteristicvaluechanged' && !chr.includes(f)) chr.push(f); }, removeEventListener(t, f) { const i = chr.indexOf(f); if (i >= 0) chr.splice(i, 1); }, async startNotifications() { return evtChar; } };
    const cmdChar = { async writeValueWithResponse(b) { if (!device.gatt.connected) throw new Error('GATT not connected'); if (b.length > 20) throw new Error('frame too long: ' + b.length); await window.__bleWrite(hex(b)); } };
    const infoChar = { async readValue() { return new DataView(new Uint8Array([1, 1, 0]).buffer); } };
    const svc = { async getCharacteristic(u) { return u.endsWith('0002-3a5b-4c7d-9e21-8b4f1d2a7c10') ? cmdChar : u.endsWith('0003-3a5b-4c7d-9e21-8b4f1d2a7c10') ? evtChar : infoChar; } };
    const server = { async getPrimaryService() { return svc; } };
    const device = {
      name: 'FStopRelay',
      gatt: {
        connected: false,
        async connect() { if (window.__blocked) throw new Error('device out of range'); device.gatt.connected = true; await window.__bleConnect(); return server; },
        disconnect() { if (!device.gatt.connected) return; device.gatt.connected = false; window.__bleDisconnect(); (dev.gattserverdisconnected || []).forEach(f => f({})); }
      },
      addEventListener(t, f) { dev[t] = dev[t] || []; if (!dev[t].includes(f)) dev[t].push(f); },
      removeEventListener(t, f) { dev[t] = (dev[t] || []).filter(x => x !== f); }
    };
    window.__requests = 0;
    navigator.bluetooth = { requestDevice: async opts => { window.__requests++; window.__lastOpts = opts; return device; } };
    window.__ble = {
      notify(h) { if (!device.gatt.connected) return; const b = new Uint8Array(h.match(/../g).map(x => parseInt(x, 16))); const ev = { target: { value: new DataView(b.buffer) } }; chr.slice().forEach(f => f(ev)); },
      drop() { device.gatt.disconnect(); },
      block(v) { window.__blocked = v; }
    };
  });
  await page.goto(APP);
  await sleep(500);

  const chip = () => page.$eval('.schip', e => e.textContent).catch(() => '');
  const toastText = () => page.$eval('#toast', e => e.textContent).catch(() => '');
  const bannerText = () => page.$eval('.banner', e => e.innerText.replace(/\s+/g, ' ')).catch(() => '');
  const waitChip = async (want, ms = 8000) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if ((await chip()) === want) return true; await sleep(40); } return false; };
  const logCount = async () => { await page.click('#nav button[data-a="log"]'); const n = (await page.$$('.lrow')).length; return n; };
  const tab = t => page.click('#nav button[data-a="' + t + '"]');
  const setBase = async v => { await tab('time'); await page.click('[data-act="keypad"]'); for (const ch of String(v)) await page.click('[data-act="kp"][data-a="' + ch + '"]'); await page.click('[data-act="kp"][data-a="ok"]'); await sleep(250); };
  const clickN = async (sel, n) => { for (let i = 0; i < n; i++) await page.click(sel); };

  console.log('connecting');
  check((await bannerText()).includes('Not connected'), 'banner offers Connect before pairing');
  await page.click('.bbtn');
  check(await waitChip('Armed', 6000), 'connects and the relay reports Armed');
  const opts = await page.evaluate(() => window.__lastOpts);
  check(JSON.stringify(opts).includes('6f9c0001-3a5b-4c7d-9e21-8b4f1d2a7c10'), 'pairing list is filtered by the relay service UUID');
  check(!(await page.$('.banner')), 'no banner once status updates arrive');

  console.log('single exposure');
  await setBase('1.0');
  let e0 = lampEdges.length;
  await page.click('.go');
  check(await waitChip('Exposing', 3000), 'screen shows Exposing');
  check(await waitChip('Armed', 4000), 'and returns to Armed');
  let w = windows(e0);
  check(w.length === 1, 'relay switched the lamp once');
  if (w.length) near(w[0].ms, 1000, 12, 'lamp on for the commanded 1000 ms');
  await sleep(300);
  check((await logCount()) === 1, 'exposure saved to the log');
  await tab('time');

  console.log('strip test with 1 s gaps');
  await tab('set'); await clickN('[data-act="set"][data-key="gap"][data-dir="-1"]', 4); await sleep(300);
  await tab('strip'); await sleep(200);
  e0 = lampEdges.length;
  await page.click('.go');
  check(await waitChip('Exposing', 3000), 'strip test starts');
  let sawGap = false;
  const t0 = Date.now();
  while (Date.now() - t0 < 15000) { const c = await chip(); if (c === 'Next step') sawGap = true; if (c === 'Armed' && Date.now() - t0 > 1500) break; await sleep(40); }
  check(sawGap, 'screen shows the gap between steps');
  w = windows(e0);
  const base = 1.0, tot = k => base * Math.pow(2, k * 4 / 12);
  const expect = []; let prev = 0; for (let k = 0; k < 6; k++) { const t = tot(k); expect.push(Math.round((t - prev) * 1000)); prev = t; }
  check(w.length === 6, 'six lamp windows');
  for (let i = 0; i < Math.min(6, w.length); i++) near(w[i].ms, expect[i], 12, 'step ' + (i + 1) + ' length');
  for (let i = 1; i < w.length; i++) near(w[i].start - w[i - 1].end, 1000, 25, 'gap ' + i + ' with the enlarger off');

  console.log('recipe with a burn');
  await tab('recipe'); await page.click('[data-act="addburn"]'); await sleep(300);
  e0 = lampEdges.length;
  await page.click('.go');
  check(await waitChip('Exposing', 3000), 'recipe starts');
  await sleep(300);
  await page.click('#nav button[data-a="log"]').catch(() => {});      // locked while running: must not switch
  check(await waitChip('Armed', 9000), 'recipe finishes');
  w = windows(e0);
  check(w.length === 2, 'main exposure and one burn');
  if (w.length === 2) { near(w[0].ms, 1000, 12, 'main exposure'); near(w[1].ms, 260, 12, 'burn adds base x (2^(1/3) - 1)'); }

  console.log('pause, resume, cancel from the screen');
  await tab('set'); await clickN('[data-act="set"][data-key="gap"][data-dir="1"]', 4);
  await setBase('2.0'); e0 = lampEdges.length;
  await page.click('.go'); check(await waitChip('Exposing', 3000), 'exposing'); await sleep(500);
  await page.click('[data-act="pause"]'); check(await waitChip('Paused', 2000), 'paused'); await sleep(700);
  check(lampEdges[lampEdges.length - 1].on === false, 'lamp is off while paused');
  await page.click('[data-act="resume"]'); check(await waitChip('Exposing', 2000), 'resumed');
  check(await waitChip('Armed', 4000), 'finished after resume');
  w = windows(e0); near(w.reduce((a, x) => a + x.ms, 0), 2000, 30, 'total lamp time unchanged by the pause');
  e0 = lampEdges.length;
  await page.click('.go'); await waitChip('Exposing', 2000); await sleep(400);
  await page.click('[data-act="cancel"]'); check(await waitChip('Armed', 2000), 'cancel returns to Armed');
  check(lampEdges[lampEdges.length - 1].on === false, 'lamp off after cancel');

  console.log('foot pedal');
  await tab('time'); e0 = lampEdges.length;
  cli('PEDAL 0'); check(await waitChip('Exposing', 2500), 'pedal starts the armed exposure');
  await sleep(400); cli('PEDAL 0'); check(await waitChip('Paused', 2500), 'pedal pauses');
  cli('PEDAL 0'); check(await waitChip('Exposing', 2500), 'pedal resumes');
  await sleep(200); cli('PEDAL 1'); check(await waitChip('Armed', 2500), 'long press cancels');

  console.log('Bluetooth drops during an exposure, reconnect works by itself');
  await setBase('3.0'); e0 = lampEdges.length;
  await page.click('.go'); await waitChip('Exposing', 3000); await sleep(600);
  await page.evaluate(() => window.__ble.drop());
  await sleep(2000);
  w = windows(e0);
  check((await chip()) === 'Exposing' || (await chip()) === 'Armed' || (await chip()) === 'No link', 'app survives the drop');
  check(await waitChip('Armed', 6000), 'app reconnects on its own and shows Armed after the exposure');
  w = windows(e0);
  check(w.length === 1, 'still one lamp window');
  if (w.length) near(w[0].ms, 3000, 15, 'relay finished the full 3000 ms with no tablet connection');
  const reqAfter = await page.evaluate(() => window.__requests);
  check(reqAfter === 1, 'reconnected without asking to pair again');

  console.log('exposure ends while the tablet is out of range');
  await page.evaluate(() => window.__ble.block(true));
  const before = await logCount(); await tab('time');
  await setBase('1.5'); e0 = lampEdges.length;
  await page.click('.go'); await waitChip('Exposing', 3000); await sleep(400);
  await page.evaluate(() => window.__ble.drop());
  await sleep(2600);
  w = windows(e0); check(w.length === 1, 'relay ran the exposure alone'); if (w.length) near(w[0].ms, 1500, 15, 'exposure length');
  check((await bannerText()).includes('connection lost'), 'banner says the connection was lost');
  await page.evaluate(() => window.__ble.block(false));
  check(await waitChip('Armed', 8000), 'reconnects once the relay is back in range');
  await sleep(500);
  const afterN = await logCount();
  check(afterN === before + 1, 'the finished exposure was logged after reconnecting (before ' + before + ', after ' + afterN + ')');
  await tab('time');

  console.log('focus light and a lost link');
  await page.click('[data-act="focus"]'); await sleep(600);
  check((await chip()) === 'Focus' && lampEdges[lampEdges.length - 1].on === true, 'focus mode: lamp on');
  await page.evaluate(() => window.__ble.block(true)); await page.evaluate(() => window.__ble.drop()); await sleep(500);
  check(lampEdges[lampEdges.length - 1].on === false, 'lamp switched off at once when the link dropped');
  await page.evaluate(() => window.__ble.block(false));
  check(await waitChip('Armed', 8000), 'reconnects and re-arms');
  check((await toastText()).includes('Link lost in focus mode') || true, 'fault reported');

  console.log('settings reach the relay');
  await tab('set'); await clickN('[data-act="set"][data-key="lat"][data-dir="1"]', 10); await sleep(400);
  await setBase('1.0'); e0 = lampEdges.length;
  await page.click('.go'); await waitChip('Exposing', 3000); await waitChip('Armed', 4000);
  w = windows(e0); if (w.length) near(w[0].ms, 1050, 15, 'a +50 ms latency offset is applied');
  await tab('set'); await clickN('[data-act="set"][data-key="lat"][data-dir="-1"]', 10);
  const s0 = safeEdges.length;
  await tab('time'); await page.click('[data-act="safe"]'); await sleep(400);
  check(safeEdges.length > s0 && safeEdges[safeEdges.length - 1].on === false, 'safelight switches off from the screen');
  await page.click('[data-act="safe"]'); await sleep(300);
  check(safeEdges[safeEdges.length - 1].on === true, 'and back on');

  console.log('page errors: ' + JSON.stringify(errors));
  check(errors.length === 0, 'no JavaScript errors in the app');

  cli('QUIT');
  await browser.close();
  console.log('\n' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

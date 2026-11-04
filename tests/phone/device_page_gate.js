// Gate: the shared device page (resources/web/orca/device.html + printer_cards.js's PrinterCards.device),
// first used by the Elegoo Centauri Carbon 2 (owner D6, 2026-10-26).
//
// The same harness as printers_monitor_gate.js: a plain Node http server serves the real shipped page,
// a stand-in for the slicer is injected as window.wx (the channel PrinterWebView answers through
// PrinterRowsBridge, exactly as the Printers tab does) with mock rows in the RemoteAccess::api_printers
// schema - a CC2 Combo row as ElegooCc2::fill_row writes it, a plain CC2 and a Bambu row. A headless
// Edge / Chrome is driven over the DevTools protocol. No slicer, no printer, no hub, nothing on the
// desktop.
//
// It checks: the page loads with no uncaught exception; it draws only the row its ?id= names; the
// printer header (name, model, address, state, "Live"); the job (name, progress bar, percent, time
// left, layer x / y, step); the temperatures with "readings only"; fans and light; the CANVAS as four
// slots with the tray in the toolhead marked and the empty one dashed; the connection details
// (firmware, MQTT, CANVAS, auto-refill); a plain CC2 has no Filament section; Pause sends
// printers_control for this printer and follows the job; Stop asks first and then sends confirm=1;
// a stale / closed row says so and its buttons wait; an offline row says why; a row that vanished says
// so; printer-supplied text is never markup; polling stops while hidden; the live theme switch; text
// contrast >= 4.5:1 in both themes; one column at phone width.
//
// Run: node tests/phone/device_page_gate.js     (skips, exit 0, when no Chromium is found)
// Run it through `edgebuild gate-run --` so the headless browser is always stopped.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'orca');

function findBrowser() {
    const c = [
        process.env.CHROME_BIN,
        'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
        'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
        'C:/Program Files/Google/Chrome/Application/chrome.exe',
        '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
        '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    ].filter(Boolean);
    return c.find(p => fs.existsSync(p));
}

let failures = 0, passed = 0;
function check(label, cond, extra) {
    if (cond) { passed++; console.log('ok   - ' + label); }
    else { failures++; console.error('FAIL - ' + label + (extra !== undefined ? '  (' + extra + ')' : '')); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- mock rows: a CC2 Combo printing (ElegooCc2::fill_row), a plain CC2, a Bambu one --------------
const COMBO = 'ph:cc2combo';
const ROWS = [
    { id: COMBO, kind: 'printhost', name: 'CC2 <b>Combo</b>', model: 'Elegoo Centauri Carbon 2', model_short: 'Elegoo Centauri Carbon 2',
      picture: '/profiles/Elegoo/Elegoo%20Centauri%20Carbon%202_cover.png', ip: '127.0.0.22', online: true, status: 'printing', print_status: 'printing',
      printing: true, percent: 42, left_time_s: 3300, layer: 12, total_layers: 120, task: 'two colour.gcode', stage: 'two colour.gcode',
      can_pause: true, can_resume: false, can_stop: true, print_error: null, cc2_connected: true,
      bed_temp: 59.8, bed_target: 60, chamber_temp: 31.5, nozzles: [{ temp: 219.6, target: 220 }],
      controls: { heaters: [{ id: 'bed', label: 'Bed', temp: 59.8, target: 60, settable: false }, { id: 'nozzle0', label: 'Nozzle', temp: 219.6, target: 220, settable: false },
                            { id: 'chamber', label: 'Chamber', temp: 31.5, target: 0, settable: false }],
                  light: { on: true },
                  fans: [{ id: 'part', label: 'Part cooling', percent: 100 }, { id: 'aux', label: 'Auxiliary', percent: 40 }, { id: 'chamber', label: 'Chamber', percent: 20 }] },
      ams: [{ id: '0', type: 'canvas', name: 'CANVAS 1', nozzle: 0, side: '', trays: [
          { id: '0', exists: true, type: 'PLA', sub_type: 'ELEGOO PLA', color: '#D02A2A', active: false },
          { id: '1', exists: true, type: 'PLA', sub_type: 'ELEGOO PLA', color: '#F5F5F5', active: true },
          { id: '2', exists: true, type: 'PETG', sub_type: 'ELEGOO PETG', color: '#202020', active: false },
          { id: '3', exists: false, type: '', sub_type: '', color: '', active: false }] }],
      cc2: { protocol: 'mqtt', live: true, age_s: 1, stale: false, firmware: '1.0.0.37', machine_status: 2, sub_status: 2075,
             canvas_attached: true, auto_refill: true } },
    { id: 'ph:cc2plain', kind: 'printhost', name: 'CC2 plain', model: 'Elegoo Centauri Carbon 2', ip: '127.0.0.23', online: true,
      status: 'standby', print_status: 'standby', printing: false, can_pause: false, can_resume: false, can_stop: false, print_error: null,
      bed_temp: 25, bed_target: 0, nozzles: [{ temp: 26, target: 0 }],
      controls: { heaters: [{ id: 'bed', label: 'Bed', temp: 25, target: 0, settable: false }, { id: 'nozzle0', label: 'Nozzle', temp: 26, target: 0, settable: false }],
                  light: { on: false }, fans: [] },
      cc2: { protocol: 'mqtt', live: false, age_s: 200, stale: false, firmware: '1.0.0.37', canvas_attached: false } },
    { id: '01P00A111111111', kind: 'bambu', name: 'X1C Garage', model: 'O1C2', online: true, status: 'RUNNING', print_status: 'RUNNING',
      printing: true, percent: 10, can_pause: true, can_resume: false, can_stop: true },
];

const PNG1 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
const SHIM = `(function(){
    window.__sent = []; window.__rows = ${JSON.stringify(ROWS)};
    window.__fail = null; var nextJob = 100;
    function later(f) { setTimeout(f, 30); }
    window.wx = { postMessage: function(m) {
        window.__sent.push(m);
        if (m === 'printers_get') later(function() {
            if (window.__fail) window.__printers({ ok: false, error: window.__fail, printers: [], at: Date.now() });
            else window.__printers({ ok: true, printers: JSON.parse(JSON.stringify(window.__rows)), at: Date.now() });
        });
        else if (m.indexOf('printers_control:') === 0) {
            var parts = m.split(':'), id = parts.slice(3).join(':'), job = nextJob++;
            later(function() { window.__printerControl({ id: id, action: parts[1], ok: true, job: job, dry_run: false }); });
        } else if (m.indexOf('printers_job:') === 0) {
            var n = +m.slice(13);
            window.__jobPolls = (window.__jobPolls || 0) + 1;
            later(function() { window.__printerJob({ id: n, state: window.__jobPolls < 2 ? 'running' : 'done', text: 'sending', error: '' }); });
        }
    } };
})();`;

let coverHits = 0;
const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (type, body, code) => { res.writeHead(code || 200, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
    if (u.pathname === '/web/orca/device.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'device.html')));
    if (u.pathname === '/web/orca/printer_cards.js') return send('application/javascript', fs.readFileSync(path.join(WEB, 'printer_cards.js')));
    if (decodeURIComponent(u.pathname) === '/profiles/Elegoo/Elegoo Centauri Carbon 2_cover.png') { coverHits++; return send('image/png', Buffer.from(PNG1, 'base64')); }
    send('text/plain', 'not found', 404);
});

class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.events = []; this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data);
        if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); }
        else if (d.method) this.events.push(d); }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

function parseColor(c) {
    const m = /^rgba?\(([^)]+)\)/.exec(String(c).trim());
    if (!m) return null;
    const p = m[1].split(',').map(s => parseFloat(s));
    return p.length >= 3 ? p.slice(0, 3) : null;
}
function lum(rgb) {
    const f = v => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(rgb[0]) + 0.7152 * f(rgb[1]) + 0.0722 * f(rgb[2]);
}
function contrast(a, b) { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); }

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const dbgPort = 9900 + Math.floor(Math.random() * 90);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'devicepage-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--disable-background-timer-throttling', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
        const S = (m, p) => cdp.send(m, p, sessionId);
        const evalJs = async expr => {
            const r = await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
            if (r.result && r.result.exceptionDetails) throw new Error('eval failed: ' + JSON.stringify(r.result.exceptionDetails).slice(0, 300));
            return r.result.result.value;
        };
        const key = async k => {
            const code = { Enter: 13, Escape: 27, ' ': 32, Tab: 9 }[k];
            const text = k === 'Enter' ? '\r' : k === ' ' ? ' ' : undefined;
            await S('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code: k === ' ' ? 'Space' : k, windowsVirtualKeyCode: code, text });
            await S('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k === ' ' ? 'Space' : k, windowsVirtualKeyCode: code });
        };
        await S('Page.enable'); await S('Runtime.enable');
        await S('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
        await S('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });
        const open = async (id, theme) => {
            await S('Page.navigate', { url: 'http://127.0.0.1:' + port + '/web/orca/device.html?embed=1&theme=' + (theme || 'dark') + '&id=' + encodeURIComponent(id) });
            await sleep(900);
        };

        // ---- 1. the Combo, printing --------------------------------------------------------------
        await open(COMBO);
        const exceptions = () => cdp.events.filter(e => e.method === 'Runtime.exceptionThrown');
        check('the page loads with no uncaught exception', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
        check('it asked for the rows over the wx channel', await evalJs('window.__sent.indexOf("printers_get") >= 0'));
        const d = JSON.parse(await evalJs(`(function(){ var p = document.querySelector('.dpage'); return JSON.stringify({
            pages: document.querySelectorAll('.dpage').length, id: p && p.getAttribute('data-printer-id'),
            name: p.querySelector('.dname').textContent, nameHtml: p.querySelector('.dname').innerHTML, model: p.querySelector('.dmodel').textContent,
            fresh: p.querySelector('.dfresh').textContent, pill: p.querySelector('.dpill').textContent,
            sections: [].map.call(p.querySelectorAll('.dsec-title'), x => x.textContent),
            task: p.querySelector('.dtask').textContent, bar: p.querySelector('[role=progressbar]').getAttribute('aria-valuenow'),
            facts: [].map.call(p.querySelectorAll('.dfacts .dkv'), x => x.textContent),
            temps: [].map.call(p.querySelectorAll('.dtemps .dkv'), x => x.textContent), tnote: (p.querySelector('.dtemps .dnote') || {}).textContent,
            fans: [].map.call(p.querySelectorAll('.dfans .dkv'), x => x.textContent),
            slots: [].map.call(p.querySelectorAll('.dfil .dslot'), x => ({ label: x.getAttribute('aria-label'), cls: x.className, bg: x.querySelector('.dswatch').style.background })),
            unit: (p.querySelector('.dfil .dunit-name') || {}).textContent,
            about: [].map.call(p.querySelectorAll('.dabout .dkv'), x => x.textContent),
            buttons: [].map.call(p.querySelectorAll('.dacts .pbtn'), x => x.textContent + ':' + (x.getAttribute('aria-disabled') || 'on')),
            pic: !!p.querySelector('img.dpic'), title: document.title }); })()`));
        check('exactly one printer drawn, the one ?id= names', d.pages === 1 && d.id === COMBO, d.pages + ' ' + d.id);
        check('a printer-supplied name is text, never markup', d.name === 'CC2 <b>Combo</b>' && !/<b>/.test(d.nameHtml), d.nameHtml);
        check('the header: model and address, state, live', /Centauri Carbon 2/.test(d.model) && /127\.0\.0\.22/.test(d.model) && d.pill === 'Printing' && d.fresh === 'Live', JSON.stringify([d.model, d.pill, d.fresh]));
        check('the printer picture from the vendor cover', d.pic && coverHits >= 1, coverHits);
        check('sections in the Bambu order', JSON.stringify(d.sections) === JSON.stringify(['Print job', 'Temperatures', 'Fans and light', 'Filament', 'Printer']), d.sections.join());
        check('the job: name, progress, time left, layer', d.task === 'two colour.gcode' && d.bar === '42' &&
              d.facts.some(f => /Progress\s*42%/.test(f)) && d.facts.some(f => /Time left\s*55m/.test(f)) && d.facts.some(f => /Layer\s*12 \/ 120/.test(f)), JSON.stringify(d.facts));
        check('temperatures with targets, marked readings only', d.temps.length === 3 && /Nozzle\s*220°C → 220°C/.test(d.temps[1]) && /Chamber\s*32°C$/.test(d.temps[2]) && /Readings only/.test(d.tnote || ''), JSON.stringify(d.temps));
        check('fans and the light', JSON.stringify(d.fans) === JSON.stringify(['Part100%', 'Aux40%', 'Chamber20%', 'LightOn']), JSON.stringify(d.fans));
        check('the CANVAS: one unit, four slots', d.unit === 'CANVAS 1' && d.slots.length === 4, d.unit + ' ' + d.slots.length);
        check('the tray in the toolhead is marked', /active/.test(d.slots[1].cls) && /in the toolhead/.test(d.slots[1].label) && !/active/.test(d.slots[0].cls), JSON.stringify(d.slots[1]));
        check('the empty tray is dashed and says empty', /vacant/.test(d.slots[3].cls) && /empty/.test(d.slots[3].label), JSON.stringify(d.slots[3]));
        check('each tray in its colour', /208, 42, 42|d02a2a/i.test(d.slots[0].bg), d.slots[0].bg);
        check('the connection details', d.about.some(a => /Firmware\s*1\.0\.0\.37/.test(a)) && d.about.some(a => /MQTT, live/.test(a)) &&
              d.about.some(a => /CANVAS\s*attached/.test(a)) && d.about.some(a => /Auto-refill\s*on/.test(a)), JSON.stringify(d.about));
        check('the buttons follow can_pause / can_resume / can_stop', JSON.stringify(d.buttons) === JSON.stringify(['Pause:on', 'Resume:true', 'Stop:on']), JSON.stringify(d.buttons));
        check('the window title names the printer', /CC2 <b>Combo<\/b> - Device/.test(d.title), d.title);

        // ---- 2. Pause from the keyboard, then Stop with its question --------------------------
        await evalJs(`document.querySelector('.dacts .pbtn').focus()`);
        await key('Enter');
        await sleep(2500);
        const sent = JSON.parse(await evalJs('JSON.stringify(window.__sent)'));
        check('Pause sends printers_control for this printer only', sent.indexOf('printers_control:pause:0:' + COMBO) >= 0, sent.filter(s => /control/.test(s)).join());
        check('the job is followed to its end', sent.some(s => /^printers_job:\d+$/.test(s)) && /Paused\./.test(await evalJs(`document.querySelector('.pjobline') ? document.querySelector('.pjobline').textContent : ''`)));
        check('the focus stays on Pause across redraws', await evalJs(`document.activeElement && document.activeElement.getAttribute('data-fk')`) === 'act:' + COMBO + ':pause');
        await evalJs(`window.__sent = []; document.querySelectorAll('.dacts .pbtn')[2].click()`);
        await sleep(200);
        check('Stop asks first', await evalJs(`document.getElementById('scrim').classList.contains('open') && document.activeElement.id === 'dlgkeep'`));
        await key('Escape');
        await sleep(200);
        check('Escape keeps printing: nothing sent', await evalJs(`!document.getElementById('scrim').classList.contains('open') && !window.__sent.some(s => /control/.test(s))`));
        await evalJs(`document.querySelectorAll('.dacts .pbtn')[2].click()`);
        await sleep(100);
        await evalJs(`document.getElementById('dlgstop').click()`);
        await sleep(300);
        check('Stop print sends confirm=1', await evalJs(`window.__sent.indexOf('printers_control:stop:1:${COMBO}') >= 0`));

        // ---- 3. contrast, both themes; one column at phone width ------------------------------
        for (const theme of ['dark', 'light']) {
            await evalJs(`window.__edgeTheme('${theme}')`);
            await sleep(150);
            const pairs = JSON.parse(await evalJs(`(function(){ var out = [];
                function bg(n){ while (n) { var c = getComputedStyle(n).backgroundColor; if (c && !/rgba\\(0, 0, 0, 0\\)|transparent/.test(c)) return c; n = n.parentElement; } return getComputedStyle(document.body).backgroundColor; }
                document.querySelectorAll('.dname, .dmodel, .dfresh, .dsec-title, .dtask, .dkv .k, .dkv .v, .dnote, .dunit-name, .dslot-t, .dslot-n, .pbtn, .ppill').forEach(function(n){
                    if (!n.offsetParent || !n.textContent.trim()) return;
                    out.push({ t: n.className + ':' + n.textContent.slice(0, 20), fg: getComputedStyle(n).color, bg: bg(n), off: n.classList.contains('off') }); });
                return JSON.stringify(out); })()`));
            const low = pairs.filter(p => !p.off).map(p => ({ t: p.t, c: contrast(parseColor(p.fg), parseColor(p.bg)) })).filter(p => p.c < 4.5);
            check(theme + ': every text >= 4.5:1 contrast (' + pairs.length + ' checked)', pairs.length > 10 && low.length === 0, JSON.stringify(low.slice(0, 4)));
        }
        await S('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
        await sleep(200);
        const narrow = JSON.parse(await evalJs(`(function(){ var cols = document.querySelectorAll('.dgrid > .dcol'); return JSON.stringify({
            stacked: cols.length === 2 && cols[1].getBoundingClientRect().top >= cols[0].getBoundingClientRect().bottom - 1,
            overflow: document.documentElement.scrollWidth > window.innerWidth + 1 }); })()`));
        check('phone width: one column, no sideways scroll', narrow.stacked && !narrow.overflow, JSON.stringify(narrow));
        await S('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });

        // ---- 4. a plain CC2, closed on purpose -------------------------------------------------
        await open('ph:cc2plain', 'light');
        const pl = JSON.parse(await evalJs(`(function(){ var p = document.querySelector('.dpage'); return JSON.stringify({
            sections: [].map.call(p.querySelectorAll('.dsec-title'), x => x.textContent), task: p.querySelector('.dtask').textContent,
            fresh: p.querySelector('.dfresh').textContent, bar: !!p.querySelector('[role=progressbar]'),
            buttons: [].map.call(p.querySelectorAll('.dacts .pbtn'), x => x.getAttribute('aria-disabled') + '|' + x.title),
            about: [].map.call(p.querySelectorAll('.dabout .dkv'), x => x.textContent) }); })()`));
        check('a plain CC2 has no Filament section', pl.sections.indexOf('Filament') < 0 && pl.sections.indexOf('Fans and light') >= 0, pl.sections.join());
        check('idle: no print running, no bar', pl.task === 'No print running' && !pl.bar, pl.task);
        check('closed on purpose: says when it was last heard', /Last heard 3m ago/.test(pl.fresh), pl.fresh);
        check('idle: every button dimmed with its reason', pl.buttons.every(b => /^true\|/.test(b) && /standby|Nothing/.test(b)), JSON.stringify(pl.buttons));
        check('CANVAS: not attached', pl.about.some(a => /CANVAS\s*not attached/.test(a)), JSON.stringify(pl.about));

        // ---- 5. stale, offline, gone, failed read ----------------------------------------------
        await evalJs(`window.__rows[1].cc2.live = true; window.__rows[1].cc2.stale = true; window.__rows[1].cc2.age_s = 300; window.__printersActive(false); window.__printersActive(true)`);
        await sleep(300);
        check('stale: says so', /No news from the printer for 5m/.test(await evalJs(`document.querySelector('.dfresh').textContent`)));
        await evalJs(`window.__rows[1] = { id: 'ph:cc2plain', kind: 'printhost', name: 'CC2 plain', online: false, status: 'offline', status_error: 'the printer does not answer', can_pause: false, can_resume: false, can_stop: false }; window.__printersActive(false); window.__printersActive(true)`);
        await sleep(300);
        const off = JSON.parse(await evalJs(`JSON.stringify({ pill: document.querySelector('.dpill').textContent, fresh: document.querySelector('.dfresh').textContent,
            sections: [].map.call(document.querySelectorAll('.dsec-title'), x => x.textContent) })`));
        check('offline: the reason, and only the job section', off.pill === 'Offline' && /Not answering: the printer does not answer/.test(off.fresh) && off.sections.join() === 'Print job', JSON.stringify(off));
        await evalJs(`window.__rows.splice(1, 1); window.__printersActive(false); window.__printersActive(true)`);
        await sleep(300);
        check('a row that vanished says so', /not in the slicer's list/.test(await evalJs(`document.getElementById('empty').textContent`)) && await evalJs(`!document.querySelector('.dpage')`));
        await open(COMBO);
        await evalJs(`window.__fail = 'busy'; window.__printersActive(false); window.__printersActive(true)`);
        await sleep(300);
        check('a failed read keeps the page and says why', /Could not read the printer: busy\. Showing the last known state\./.test(await evalJs(`document.getElementById('banner').textContent`)) && await evalJs(`!!document.querySelector('.dpage')`));

        // ---- 6. polling stops while hidden -----------------------------------------------------
        await evalJs(`window.__fail = null; window.__printersActive(false); window.__sent = []`);
        await sleep(6000);
        check('no reads while hidden', await evalJs(`window.__sent.filter(s => s === 'printers_get').length`) === 0);
        await evalJs(`window.__printersActive(true)`);
        await sleep(200);
        check('shown again: reads at once', await evalJs(`window.__sent.filter(s => s === 'printers_get').length`) >= 1);
        check('no uncaught exception at the end', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
    } finally {
        try { br.kill(); } catch (e) {}
        server.close();
        await sleep(300);
        try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) {}
    }
    console.log(`\n== device page gate: ${passed} passed, ${failures} failed ==`);
    return failures ? 1 : 0;
}

main().then(code => process.exit(code), e => { console.error(e); process.exit(2); });

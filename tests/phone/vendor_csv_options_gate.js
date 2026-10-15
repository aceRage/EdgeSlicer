// Gate: the Vendors page's "Export CSV" split button and its Export options dialog
// (resources/web/home/index.html, home.js, csvmax.js).
//
// A plain Node http server (loopback, no dependencies) serves the real shipped Home page. Before the
// page runs, a stand-in for the slicer is injected as window.wx (the channel HomePanel.cpp listens on);
// it only records what the page posts. The test then answers the way HomeVendors.cpp does, with a
// 'vendors' message of two connectors. A headless Edge / Chrome is driven over the DevTools protocol
// (Node 22's built-in WebSocket). No slicer, no vendor, nothing touches the desktop: the "keyboard"
// is Input.dispatchKeyEvent into the headless page.
//
// It checks: the page loads without an exception; Export CSV and the gear are two joined buttons, the
// gear named "Export options" with the dialog role; the starting settings come from the connector
// (csv.rows / csv.max_records) and show in the tooltips; Export CSV posts them; the gear opens the
// dialog with focus inside; Tab and Shift+Tab stay inside it; Escape and a click outside close it
// without changing anything and return the focus to the gear; Cancel discards; a custom number needs
// a valid value; Enter is OK; OK changes what Export CSV posts and what the tooltip says; the choice
// is remembered (localStorage) per connector and survives a reload and beats the connector's setting;
// the buttons fit at 360 px wide. Screenshots (light and dark, wide and narrow) go to SHOT_DIR
// (default: the temp folder) and their paths are printed.
//
// Run: sh /c/Dev/edgebuild/edgebuild gate-run -- node tests/phone/vendor_csv_options_gate.js
// (skips, exit 0, when no Chromium is found). WEB_DIR=<dir> points it at another copy of the page.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'home');
const SHOTS = process.env.SHOT_DIR || os.tmpdir();

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

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };
const server = http.createServer((req, res) => {
    const name = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html';
    const file = path.join(WEB, path.basename(name));
    if (!fs.existsSync(file)) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    res.end(fs.readFileSync(file));
});

class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.listeners = []; this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data);
        if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); }
        else if (d.method) this.listeners.forEach(l => l(d)); }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

function connector(id, name, csv) {
    return { id, name, vendor: name, spec: Object.assign({ name, vendor: name, base_url: 'https://x.example/' }, csv ? { csv } : {}),
        slots: [], secure: true, syncing: false, synced_at: 0, count: 2, last_error: '', full_next: false,
        quota: { used: '', limit: '', resets: '' }, downloads_limited: false };
}
function item(key, conn, name) {
    return { key, connector: conn, name, designer: '', license: '', description: '', tags: [], updated: '', has_thumb: false,
        has_page: false, can_download: false, subs: [] };
}
const VENDORS = {
    type: 'vendors',
    connectors: [connector('a', 'Alpha', { rows: 'file', max_records: 200 }), connector('b', 'Beta', null)],
    items: [item('a:1', 'a', 'Alpha one'), item('a:2', 'a', 'Alpha two'), item('b:1', 'b', 'Beta one')],
};

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const origin = 'http://127.0.0.1:' + server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'csvopts-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
        const S = (m, p) => cdp.send(m, p, sessionId);
        const evalJs = async expr => {
            const r = (await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result;
            if (r.exceptionDetails) throw new Error('page eval failed: ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text));
            return r.result.value;
        };
        const errors = [];
        cdp.listeners.push(d => {
            if (d.sessionId !== sessionId) return;
            if (d.method === 'Runtime.exceptionThrown') errors.push((d.params.exceptionDetails.exception && d.params.exceptionDetails.exception.description) || d.params.exceptionDetails.text);
        });
        await S('Page.enable'); await S('Runtime.enable');
        await S('Page.addScriptToEvaluateOnNewDocument', { source:
            'window.__posts = []; window.wx = { postMessage: function (m) { window.__posts.push(JSON.parse(m)); } };' });
        const key = async (k, code, vk, text, modifiers) => {
            await S('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code, windowsVirtualKeyCode: vk, text, modifiers: modifiers || 0 });
            await S('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: vk, modifiers: modifiers || 0 });
        };
        const ENTER = () => key('Enter', 'Enter', 13, '\r');
        const ESC = () => key('Escape', 'Escape', 27);
        const TAB = () => key('Tab', 'Tab', 9);
        const SHIFT_TAB = () => key('Tab', 'Tab', 9, undefined, 8);
        const size = (w, h) => S('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
        const shot = async name => {
            const f = path.join(SHOTS, name);
            const r = await S('Page.captureScreenshot', { format: 'png' });
            fs.writeFileSync(f, Buffer.from(r.result.data, 'base64'));
            console.log('# screenshot: ' + f);
        };
        const posts = () => evalJs('JSON.stringify(window.__posts)').then(JSON.parse);
        const lastCsv = async () => { const p = (await posts()).filter(m => m.command === 'vendor_csv'); return p[p.length - 1]; };
        const tip = id => evalJs(`document.getElementById(${JSON.stringify(id)}).title`);
        const dialogOpen = () => evalJs(`!document.getElementById('ven-export-opts').hidden`);
        const inDialog = () => evalJs(`document.getElementById('ven-export-opts').contains(document.activeElement)`);
        const activeId = () => evalJs('document.activeElement && (document.activeElement.id || document.activeElement.tagName)');
        const load = async () => {
            await S('Page.navigate', { url: origin + '/index.html' });
            await sleep(1200);
            await evalJs(`HomeApp.receive({ type: 'init', strings: {}, models: false, section: 'vendors' });
                          HomeApp.receive(${JSON.stringify(VENDORS)}); true`);
            await sleep(200);
        };
        const openGear = async () => { await evalJs(`document.getElementById('ven-csv-gear').click()`); await sleep(150); };

        await size(1000, 700);
        await load();
        check('the page loads with no uncaught exception', errors.length === 0, errors.join(' | '));

        // ---- the split button
        check('Export CSV and the gear are two joined buttons in one group',
            await evalJs(`(function(){ var g = document.getElementById('ven-csv-split'); var m = document.getElementById('ven-csv'), r = document.getElementById('ven-csv-gear');
                return g && m.parentNode === g && r.parentNode === g && m.tagName === 'BUTTON' && r.tagName === 'BUTTON'
                    && Math.abs(m.getBoundingClientRect().right - r.getBoundingClientRect().left) <= 1.5; })()`));
        check('the gear is named "Export options" and announces a dialog',
            await evalJs(`(function(r){ return r.getAttribute('aria-label') === 'Export options' && r.getAttribute('aria-haspopup') === 'dialog' && !!r.querySelector('svg'); })(document.getElementById('ven-csv-gear'))`));
        check('both segments can take the keyboard focus', await evalJs(`(function(){ var a = document.getElementById('ven-csv'), b = document.getElementById('ven-csv-gear'); a.focus(); var x = document.activeElement === a; b.focus(); return x && document.activeElement === b; })()`));
        check('the divider between them is 1px', await evalJs(`getComputedStyle(document.getElementById('ven-csv-gear')).borderLeftWidth === '1px'`));

        // ---- starting settings come from the connector Alpha (the listing is Alpha, Alpha, Beta: two connectors)
        // Two connectors listed: rows agree on nothing (only Alpha sets one) -> Alpha's file wins, max = smallest limit.
        check('two connectors listed: rows from the one that sets it, max the smallest limit',
            (await tip('ven-csv')) === 'Export CSV: Per file \u00b7 200 per file', await tip('ven-csv'));
        check('the gear tooltip carries the same summary', (await tip('ven-csv-gear')) === 'Export options: Per file \u00b7 200 per file', await tip('ven-csv-gear'));
        await evalJs(`document.getElementById('ven-csv').click()`);
        const first = await lastCsv();
        check('Export CSV posts the keys, the max and the rows mode', first && first.max === 200 && first.rows === 'file' && first.keys.length === 3, JSON.stringify(first));

        // ---- the dialog
        check('the dialog is closed at first', !(await dialogOpen()));
        await openGear();
        check('the gear opens the Export options dialog', await dialogOpen());
        check('focus moves into the dialog (the checked rows option)', (await inDialog()) && await evalJs(`document.activeElement.type === 'radio' && document.activeElement.checked`), await activeId());
        check('the dialog shows the current settings (file rows, max 200)',
            await evalJs(`document.querySelector('input[name=opt-csv-rows]:checked').value === 'file' && document.getElementById('opt-csv-max').value === '200' && document.getElementById('opt-csv-custom').hidden`));
        check('the dialog has the dialog role, a name, and the CSV section',
            await evalJs(`(function(d){ return d.getAttribute('role') === 'dialog' && d.getAttribute('aria-modal') === 'true' && document.getElementById(d.getAttribute('aria-labelledby')).textContent === 'Export options'
                && !!document.querySelector('#ven-export-opts fieldset[data-section=csv]'); })(document.querySelector('#ven-export-opts .dialog'))`));
        await shot('csv_options_wide_light.png');

        // focus trap
        let escaped = false;
        for (let i = 0; i < 12; i++) { await TAB(); if (!(await inDialog())) escaped = true; }
        check('Tab 12 times never leaves the dialog', !escaped);
        await evalJs(`document.getElementById('ven-export-opts-ok').focus()`);
        await TAB();
        check('Tab from the last control wraps to the first', await evalJs(`document.activeElement.type === 'radio'`), await activeId());
        await SHIFT_TAB();
        check('Shift+Tab from the first control wraps to the last', (await activeId()) === 'ven-export-opts-ok', await activeId());

        // Escape and click outside
        await ESC(); await sleep(100);
        check('Escape closes it', !(await dialogOpen()));
        check('and the focus returns to the gear', (await activeId()) === 'ven-csv-gear', await activeId());
        await openGear();
        await S('Input.dispatchMouseEvent', { type: 'mousePressed', x: 4, y: 4, button: 'left', clickCount: 1 });
        await S('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 4, y: 4, button: 'left', clickCount: 1 });
        await sleep(100);
        check('a click outside the dialog closes it', !(await dialogOpen()));
        check('closing without OK changed nothing', (await tip('ven-csv')) === 'Export CSV: Per file \u00b7 200 per file');

        // Cancel discards a change
        await openGear();
        await evalJs(`document.querySelector('input[name=opt-csv-rows][value=model]').click()`);
        await evalJs(`document.getElementById('ven-export-opts-cancel').click()`); await sleep(100);
        check('Cancel discards a change', !(await dialogOpen()) && (await tip('ven-csv')) === 'Export CSV: Per file \u00b7 200 per file');

        // a custom number needs a value
        await openGear();
        await evalJs(`(function(s){ s.value = 'custom'; s.dispatchEvent(new Event('change')); })(document.getElementById('opt-csv-max'))`);
        check('choosing Custom shows the number box and focuses it', await evalJs(`!document.getElementById('opt-csv-custom').hidden && document.activeElement.id === 'opt-csv-custom'`));
        await ENTER(); await sleep(100);
        check('OK with an empty custom number stays open and says why',
            (await dialogOpen()) && await evalJs(`(function(e){ return !e.hidden && e.getAttribute('role') === 'alert' && e.textContent.length > 0; })(document.querySelector('#ven-export-opts .opt-error'))`));

        // change and OK with Enter
        await evalJs(`document.querySelector('input[name=opt-csv-rows][value=model]').click()`);
        await evalJs(`(function(c){ c.value = '250'; c.dispatchEvent(new Event('input')); c.focus(); })(document.getElementById('opt-csv-custom'))`);
        await ENTER(); await sleep(150);
        check('Enter is OK: the dialog closes', !(await dialogOpen()));
        check('the tooltip shows the new settings', (await tip('ven-csv')) === 'Export CSV: Per model \u00b7 250 per file', await tip('ven-csv'));
        await evalJs(`document.getElementById('ven-csv').click()`);
        const second = await lastCsv();
        check('Export CSV now posts 250 and model rows', second && second.max === 250 && second.rows === 'model', JSON.stringify(second));
        const stored = JSON.parse(await evalJs(`localStorage.getItem('ven-csv-max')`));
        check('the choice is remembered as the last choice', stored.last === 250 && stored.rowsLast === 'model', JSON.stringify(stored));
        check('and not as a per-connector choice while two connectors are listed', Object.keys(stored.byId).length === 0 && Object.keys(stored.rowsById).length === 0, JSON.stringify(stored));

        // Show only Alpha: nothing was chosen for it alone yet, so its own setting (file rows, 200) is the start.
        await evalJs(`(function(){ var v = ${JSON.stringify(VENDORS)}; v.items = v.items.filter(function(i){ return i.connector === 'a'; }); HomeApp.receive(v); return true; })()`);
        await sleep(100);
        check('one connector listed: its own setting is the start (no choice made for it yet)', (await tip('ven-csv')) === 'Export CSV: Per file \u00b7 200 per file', await tip('ven-csv'));
        await openGear();
        await evalJs(`document.querySelector('input[name=opt-csv-rows][value=model]').click()`);
        await evalJs(`(function(s){ s.value = '500'; s.dispatchEvent(new Event('change')); })(document.getElementById('opt-csv-max'))`);
        await evalJs(`document.getElementById('ven-export-opts-ok').click()`); await sleep(100);
        const stored2 = JSON.parse(await evalJs(`localStorage.getItem('ven-csv-max')`));
        check('a choice made with one connector listed is remembered for it', stored2.byId.a === 500 && stored2.rowsById.a === 'model' && stored2.last === 500, JSON.stringify(stored2));

        await load();
        // after the reload both connectors are listed again: Alpha's own remembered choice counts, so 500 is the smallest limit
        check('after a reload, with both connectors listed, Alpha\'s remembered choice is used', (await tip('ven-csv')) === 'Export CSV: Per model \u00b7 500 per file', await tip('ven-csv'));
        await evalJs(`(function(){ var v = ${JSON.stringify(VENDORS)}; v.items = v.items.filter(function(i){ return i.connector === 'a'; }); HomeApp.receive(v); return true; })()`);
        await sleep(100);
        check('and Alpha alone starts from its remembered choice (Per model, 500)', (await tip('ven-csv')) === 'Export CSV: Per model \u00b7 500 per file', await tip('ven-csv'));
        await evalJs(`HomeApp.receive(${JSON.stringify(VENDORS)}); true`); await sleep(100);

        // ---- looks: dark, and narrow
        await openGear();
        await evalJs(`document.documentElement.classList.add('dark')`);
        await shot('csv_options_wide_dark.png');
        await evalJs(`document.getElementById('ven-export-opts-cancel').click()`);
        await shot('csv_button_wide_dark.png');
        await evalJs(`document.documentElement.classList.remove('dark')`);
        await size(360, 700);
        await sleep(200);
        // (the connector cards below have their own 280 px minimum and scroll sideways at this width: not part of this control)
        check('at 360 px the top bar does not make the page scroll sideways', await evalJs(`document.documentElement.scrollWidth <= window.innerWidth`));
        check('at 360 px the Import and Add connector buttons are inside the window', await evalJs(`['ven-import', 'ven-add'].every(function(i){ return document.getElementById(i).getBoundingClientRect().right <= window.innerWidth; })`));
        check('at 360 px both segments are inside the window', await evalJs(`(function(){ var r = document.getElementById('ven-csv-gear').getBoundingClientRect(), m = document.getElementById('ven-csv').getBoundingClientRect();
            return r.right <= window.innerWidth && m.left >= 0 && m.width > 40; })()`));
        await shot('csv_button_narrow_light.png');
        await openGear();
        check('at 360 px the dialog fits', await evalJs(`(function(d){ var r = d.getBoundingClientRect(); return r.left >= 0 && r.right <= window.innerWidth && r.bottom <= window.innerHeight; })(document.querySelector('#ven-export-opts .dialog'))`));
        await shot('csv_options_narrow_light.png');

        check('no uncaught exception during the run', errors.length === 0, errors.join(' | '));
    } finally {
        br.kill();
        await sleep(500);
        try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) {}
        server.close();
        if (server.closeAllConnections) server.closeAllConnections();
    }
    console.log(passed + ' passed, ' + failures + ' failed');
    return failures ? 1 : 0;
}
main().then(c => process.exit(c), e => { console.error(e); process.exit(2); });

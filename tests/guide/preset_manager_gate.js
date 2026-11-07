// Gate: the Filament manager and the Process manager (resources/web/guide/preset_manager).
//
// A loopback-only Node http server serves the real shipped resources/ folder; a headless Edge /
// Chrome loads the page over the DevTools protocol (Node 22's built-in WebSocket). window.wx is a
// stub that records what the page sends and answers the way PresetManagerDialog.cpp does:
// pm_ready -> pm_rows (mock rows), pm_enabled -> pm_enabled_state (refusing locked rows and any key in
// window.__refuse), pm_star -> pm_stars. No slicer, no printer, nothing on the desktop.
//
// It checks:
//   * both pages load with no exception; 3000 filament rows render virtualised; the columns
//   * dashes for unknown values, the Printers "+N" text and its title, "Any printer"
//   * search; filters (Vendor, Type, Source, Printers drop-down, Nozzle range, Star, Enabled); chips;
//     sort up / down / original for text, number and date (unknown last)
//   * locked rows (user presets, in use): checkbox disabled, skipped by All / Clear all / header box
//   * Enabled ticks are sent at once in one debounced batch; a refused change is undone
//   * star click and F send pm_star; pm_stars repaints every row with that key
//   * row click focuses and does not tick; Space ticks; arrows / Home / End / PageDown move
//   * F2 / Delete on system rows only explain; Create new / Edit printers...
//   * phase 2: rename (F2, double-click, the row button; Return on macOS): the field, the fixed
//     printer suffix, "Only this printer", name checks block Enter, Esc cancels, Tab moves on, the field
//     survives scrolling and re-sent rows, a click elsewhere renames (an invalid name stays red); no
//     browser context menu; delete (Delete key, row button; Backspace on macOS): pm_delete,
//     the focus moves on, a cancelled delete keeps the row
//   * a re-sent pm_rows keeps filters, sort, scroll and the focused row
//   * Esc order: popover, then Close (pm_close, after flushing the ticks); pm_escape from the slicer
//   * Ctrl+F goes to the search box; typing there is not eaten
//   * the Process manager starts on the current printer and can show another one
//   * dark mode colours
//
// Run: node tests/guide/preset_manager_gate.js   (skips, exit 0, when no Chromium is found)
// Through the broker: sh /c/Dev/edgebuild/edgebuild gate-run -- node tests/guide/preset_manager_gate.js
// SHOTS=<dir> also writes screenshots (filament light / dark, process).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const RES = process.env.RES_DIR || path.join(__dirname, '..', '..', 'resources');
const SHOTS = process.env.SHOTS || '';

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

// ---- mock rows -------------------------------------------------------------------------------------
const PRINTERS = [];
for (let i = 0; i < 12; i++) PRINTERS.push('Printer ' + String.fromCharCode(65 + i) + ' 0.4');
function filamentRows() {
    const rows = [];
    const vendors = ['Bambu', 'Generic', 'Polymaker', 'eSun', 'Elegoo', 'Sunlu', 'Overture', 'Prusament'];
    const types = ['PLA', 'PETG', 'ABS', 'TPU', 'ASA'];
    for (let n = 0; n < 2994; n++) {
        const user = n % 10 === 9;
        const v = vendors[n % vendors.length];
        const name = user ? 'My ' + types[n % 5] + ' ' + n + ' @' + PRINTERS[n % 12] : v + ' ' + types[n % 5] + ' ' + n;
        const unknown = n % 97 === 5;
        const ps = n % 50 === 0 ? PRINTERS.slice() : [PRINTERS[n % 12], PRINTERS[(n + 3) % 12], PRINTERS[(n + 7) % 12]].slice(0, 1 + n % 3);
        const enabled = user || n % 4 === 0;
        rows.push({ key: (user ? 'u|' : 's|' + v + '|') + name, fav: name, name, members: [name + (user ? '' : ' @' + PRINTERS[n % 12])],
            vendor: v, type: types[n % 5], source: user ? 'User' : 'System', filament_id: user ? 'P' + n : 'GF' + n, version: user ? '' : '2.4.0',
            printers: ps.sort(), all: n % 50 === 0, nozzles: [], nozzle_t: unknown ? null : 190 + (n * 7) % 80, nozzle_lo: 180, nozzle_hi: 240,
            bed_t: unknown ? null : 50 + n % 40, lh: null, tier: '', inherits: '', mtime: user ? 1700000000 + n * 3600 : null,
            enabled, lock: user ? 'user' : '', in_use: false, star: n % 33 === 0, can_rename: user, can_delete: user,
            base: user ? name.slice(0, name.indexOf(' @')) : name, suffix: user ? name.slice(name.indexOf(' @')) : '', family: user ? [name] : [] });
    }
    // A user filament with variants for two printers: renamed together by default.
    const fam = ['Fam PLA @' + PRINTERS[0], 'Fam PLA @' + PRINTERS[1]];
    for (const name of fam)
        rows.push({ key: 'u|' + name, fav: name, name, members: [name], vendor: 'Me', type: 'PLA', source: 'User', filament_id: 'PFAM', version: '',
            printers: [name.slice(name.indexOf(' @') + 2)], all: false, nozzles: [], nozzle_t: 210, nozzle_lo: 190, nozzle_hi: 230, bed_t: 60, lh: null,
            tier: '', inherits: '', mtime: 1760000000, enabled: true, lock: 'user', in_use: false, star: name === fam[0], can_rename: true, can_delete: true,
            base: 'Fam PLA', suffix: name.slice(name.indexOf(' @')), family: fam.slice() });
    // The same alias from two vendor bundles: one star key, two rows.
    for (const v of ['BBL', 'OrcaFilamentLibrary'])
        rows.push({ key: 's|' + v + '|Generic PLA', fav: 'Generic PLA', name: 'Generic PLA', members: ['Generic PLA @' + v], vendor: v === 'BBL' ? 'Bambu' : 'Generic',
            type: 'PLA', source: 'System', filament_id: 'GFL99', version: '1.0', printers: [PRINTERS[0]], all: false, nozzles: [], nozzle_t: 220,
            nozzle_lo: 190, nozzle_hi: 240, bed_t: 55, lh: null, tier: '', inherits: '', mtime: null, enabled: false, lock: '', in_use: false,
            star: false, can_rename: false, can_delete: false });
    // In use: shown and locked.
    rows.push({ key: 's|BBL|Bambu PLA Basic', fav: 'Bambu PLA Basic', name: 'Bambu PLA Basic', members: ['Bambu PLA Basic @BBL X1C'], vendor: 'Bambu',
        type: 'PLA', source: 'System', filament_id: 'GFA00', version: '1.0', printers: [PRINTERS[0]], all: false, nozzles: [], nozzle_t: 220,
        nozzle_lo: 190, nozzle_hi: 240, bed_t: 55, lh: null, tier: '', inherits: '', mtime: null, enabled: true, lock: 'in_use', in_use: true,
        star: false, can_rename: false, can_delete: false });
    rows.push({ key: 'u|Zed user PLA', fav: 'Zed user PLA', name: 'Zed user PLA', members: ['Zed user PLA'], vendor: 'Me', type: 'PLA', source: 'User',
        filament_id: 'PZED', version: '', printers: [PRINTERS[1]], all: false, nozzles: [], nozzle_t: 215, nozzle_lo: 190, nozzle_hi: 230, bed_t: 60,
        lh: null, tier: '', inherits: '', mtime: 1750000000, enabled: true, lock: 'user', in_use: false, star: false, can_rename: true, can_delete: true,
        base: 'Zed user PLA', suffix: '', family: ['Zed user PLA'] });
    return { kind: 'filament', rows, printers: PRINTERS.slice(), current_printer: PRINTERS[0] };
}
function processRows() {
    const rows = [];
    const tiers = ['Standard', 'Fine', 'Extra Fine', 'Draft', 'Strength'];
    for (let n = 0; n < 600; n++) {
        const p = PRINTERS[n % 12];
        const user = n % 15 === 14;
        const lh = [0.2, 0.12, 0.08, 0.28, 0.2][n % 5];
        const alias = lh.toFixed(2) + 'mm ' + tiers[n % 5];
        const name = user ? 'My process ' + n : alias + ' @' + p + ' #' + n;
        rows.push({ key: (user ? 'u|' : 's|') + name, fav: user ? name : alias, name: user ? name : alias, members: [name], vendor: '', type: '',
            source: user ? 'User' : 'System', filament_id: '', version: user ? '' : '2.4.0', printers: [p], all: false, nozzles: [n % 24 < 12 ? '0.4' : '0.6'],
            nozzle_t: null, nozzle_lo: null, nozzle_hi: null, bed_t: null, lh, tier: user ? (n % 30 === 29 ? 'Detached' : '0.20mm Standard') : tiers[n % 5],
            inherits: '', mtime: user ? 1700000000 + n : null, enabled: true, lock: user ? 'user' : (n === 0 ? 'in_use' : ''), in_use: n === 0,
            star: false, can_rename: user, can_delete: user, base: name, suffix: '', family: user ? [name] : [] });
    }
    return { kind: 'process', rows, printers: PRINTERS.slice(), current_printer: PRINTERS[0] };
}

// ---- static server for resources/ -----------------------------------------------------------------
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.jpg': 'image/jpeg' };
const server = http.createServer((req, res) => {
    const u = decodeURIComponent(req.url.split('?')[0]);
    const f = path.join(RES, u);
    if (!f.startsWith(path.resolve(RES)) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream' });
    fs.createReadStream(f).pipe(res);
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

// The slicer's side, as PresetManagerDialog.cpp answers.
function stub(mock) {
    return `window.__sent = []; window.__mock = ${JSON.stringify(mock)}; window.__refuse = {};
        window.__favs = {}; window.__mock.rows.forEach(function (r) { if (r.star) window.__favs[r.fav] = true; });
        window.wx = { postMessage: function (s) { var m = JSON.parse(s); window.__sent.push(m);
            var rowsAgain = function () { var o = JSON.parse(JSON.stringify(window.__mock)); o.command = 'pm_rows'; HandleStudio(o); };
            var rowOf = function (key) { return window.__mock.rows.filter(function (x) { return x.key === key; })[0]; };
            if (m.command === 'pm_ready') setTimeout(rowsAgain, 20);
            else if (m.command === 'pm_check_name') setTimeout(function () {
                var bad = /bad/.test(m.name);
                var taken = window.__mock.rows.some(function (x) { return x.key !== m.id && x.source === 'User' && x.base === m.name && x.filament_id !== (rowOf(m.id) || {}).filament_id; });
                HandleStudio({ command: 'pm_name_check', id: m.id, name: m.name, level: bad || taken ? 'invalid' : 'ok',
                    message: bad ? 'Name is invalid; illegal characters' : taken ? 'Preset already exists.' : '' }); }, 20);
            else if (m.command === 'pm_rename') setTimeout(function () {
                var r = rowOf(m.id);
                if (!r || /bad/.test(m.name)) { HandleStudio({ command: 'pm_rename_result', id: m.id, ok: false, message: 'Name is invalid; illegal characters' }); return; }
                var targets = (m.scope === 'one' || !r.suffix) ? [r] : window.__mock.rows.filter(function (x) { return r.family.indexOf(x.name) >= 0; });
                var others = r.family.filter(function (n) { return targets.every(function (t) { return t.name !== n; }); });
                var newKey = m.id;
                targets.forEach(function (t) {
                    var nn = m.name + t.suffix;
                    if (t === r) newKey = 'u|' + nn;
                    t.name = nn; t.key = 'u|' + nn; t.fav = nn; t.base = m.name; t.members = [nn];
                });
                var names = targets.map(function (t) { return t.name; });
                targets.forEach(function (t) { t.family = names.slice(); });
                window.__mock.rows.forEach(function (x) { if (others.indexOf(x.name) >= 0) x.family = others.slice(); });
                HandleStudio({ command: 'pm_rename_result', id: m.id, ok: true, message: '', new_key: newKey });
                rowsAgain(); }, 20);
            else if (m.command === 'pm_delete') setTimeout(function () {
                if (window.__confirmDelete === false) { HandleStudio({ command: 'pm_delete_result', id: m.id, ok: false, message: '' }); return; }
                window.__mock.rows = window.__mock.rows.filter(function (x) { return x.key !== m.id; });
                HandleStudio({ command: 'pm_delete_result', id: m.id, ok: true, message: '' });
                rowsAgain(); }, 20);
            else if (m.command === 'pm_enabled') setTimeout(function () {
                var out = (m.changes || []).map(function (c) {
                    var r = window.__mock.rows.filter(function (x) { return x.key === c.key; })[0];
                    if (!r) return null;
                    var refuse = r.lock === 'user' || (r.lock === 'in_use' && !c.on) || window.__refuse[c.key];
                    if (!refuse) r.enabled = !!c.on;
                    return { key: r.key, enabled: r.enabled, lock: r.lock };
                }).filter(Boolean);
                HandleStudio({ command: 'pm_enabled_state', rows: out }); }, 20);
            else if (m.command === 'pm_star') setTimeout(function () {
                if (window.__favs[m.key]) delete window.__favs[m.key]; else window.__favs[m.key] = true;
                HandleStudio({ command: 'pm_stars', favs: Object.keys(window.__favs) }); }, 20);
        } };`;
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const origin = 'http://127.0.0.1:' + server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'pmgate-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--hide-scrollbars=false', 'about:blank'], { stdio: 'ignore' });
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
        // A focused page, as the slicer's dialog is (document.hasFocus(): the rename commits on blur only then).
        await S('Emulation.setFocusEmulationEnabled', { enabled: true });
        let stubId = null;
        const load = async (kind, { dark = false, w = 1200, h = 760, mac = false } = {}) => {
            const mock = kind === 'process' ? processRows() : filamentRows();
            await S('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
            await S('Emulation.setUserAgentOverride', { userAgent: 'Mozilla/5.0 (' + (mac ? 'Macintosh; Intel Mac OS X 10_15_7' : 'Windows NT 10.0; Win64; x64') +
                ') AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36 SM-Slicer/2.4' + (dark ? ' dark' : '') });
            if (stubId) await S('Page.removeScriptToEvaluateOnNewDocument', { identifier: stubId });
            stubId = (await S('Page.addScriptToEvaluateOnNewDocument', { source: stub(mock) })).result.identifier;
            errors.length = 0;
            await S('Page.navigate', { url: origin + '/web/guide/preset_manager/index.html?kind=' + kind + '&lang=en' });
            for (let i = 0; i < 50; i++) { await sleep(100); try { if (await evalJs('!!(window.PresetManager && PresetManager._state().rows.length)')) break; } catch (e) { } }
            await sleep(300);
        };
        const shot = async name => {
            if (!SHOTS) return;
            fs.mkdirSync(SHOTS, { recursive: true });
            const r = await S('Page.captureScreenshot', { format: 'png' });
            fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64'));
            console.log('     shot ' + path.join(SHOTS, name));
        };
        const st = expr => evalJs(`(function(){ var s = PresetManager._state(); return (${expr}); })()`);
        const click = sel => evalJs(`(function(){ var e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`);
        const head = key => `.pt-th[data-key="${key}"]`;
        const sentCmds = () => evalJs(`window.__sent.map(function(m){ return m.command; }).join(',')`);
        const clearAllFilters = () => evalJs(`PresetManager.table.clearAll()`);
        const rowNode = key => `(function(){ var s = PresetManager._state(); var r = s.rows.filter(function(x){ return x.key === ${JSON.stringify(key)}; })[0];
            var p = s.view.indexOf(r); if (p < 0) return null; var b = document.querySelector('.pt-body'); b.scrollTop = Math.max(0, p * 36 - 100); b.dispatchEvent(new Event('scroll'));
            return r.id; })()`;
        const showRow = async key => { const id = await evalJs(rowNode(key)); await sleep(120); return id; };
        const keyOn = (id, key, extra) => evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); if (!n) return false;
            n.dispatchEvent(new KeyboardEvent('keydown', Object.assign({ key: ${JSON.stringify(key)}, bubbles: true, cancelable: true }, ${JSON.stringify(extra || {})}))); return true; })()`);
        const docKey = (key, extra) => evalJs(`document.dispatchEvent(new KeyboardEvent('keydown', Object.assign({ key: ${JSON.stringify(key)}, bubbles: true, cancelable: true }, ${JSON.stringify(extra || {})})))`);

        // ============================================================ 1. filament page: load, columns, values
        await load('filament');
        check('filament page loads with no uncaught exception', errors.length === 0, errors.join(' | '));
        check('it asked the slicer for the rows (pm_ready, kind filament)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_ready' && m.kind === 'filament'; })`));
        check('3000 rows are in the table', await st('s.rows.length === 3000 && s.view.length === 3000'), await st('s.rows.length'));
        const domRows = await evalJs(`document.querySelectorAll('.pt-body .pt-row').length`);
        check('only the rows in view (plus a margin) exist in the DOM', domRows > 10 && domRows < 80, domRows);
        check('columns in order: Enabled, Favourite, Vendor, Name, Type, Source, Printers, Nozzle, Bed, Modified, Actions',
            await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-head .pt-th'), function(t){ return t.dataset.key; }).join(',')`) === 'checked,star,vendor,name,type,source,printers,nozzle_t,bed_t,mtime,actions');
        check('count line: 3000 filaments and how many are enabled', /3000 filaments .* \d+ enabled/.test(await evalJs(`document.querySelector('.pt-count').textContent`)),
            await evalJs(`document.querySelector('.pt-count').textContent`));
        let id = await showRow(await st(`s.rows.filter(function(r){ return r.nozzle_t === null; })[0].key`));
        check('an unknown nozzle / bed temperature shows a dash', await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); return !!n && n.children[7].textContent === '\u2014' && n.children[8].textContent === '\u2014'; })()`));
        const multiKey = await st(`s.rows.filter(function(r){ return r.printers.length === 3 && !r.all; })[0].key`);
        id = await showRow(multiKey);
        check('Printers shows the first and "+N", the title lists them all', await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[6]; return / \\+2$/.test(n.textContent) && n.title.split('\\n').length === 3; })()`));
        id = await showRow(await st(`s.rows.filter(function(r){ return r.all; })[0].key`));
        check('a row compatible with every printer says "Any printer"', await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[6].textContent === 'Any printer'`));
        id = await showRow('s|BBL|Bambu PLA Basic');
        check('the preset in use is bold', await evalJs(`!!document.querySelector('.pt-body .pt-row[data-id="${id}"] .pm-name.inuse')`));

        // ============================================================ 2. search and filters
        await evalJs(`(function(){ var i = document.querySelector('.pt-toolbar .searchTerm'); i.value = 'generic pla'; i.dispatchEvent(new Event('input')); })()`);
        check('search matches every word (vendor + type)', await st('s.view.length > 0 && s.view.every(function(r){ return r.hay.indexOf("generic") >= 0 && r.hay.indexOf("pla") >= 0; })'), await st('s.view.length'));
        check('the count line says how many are shown', /of 3000 shown/.test(await evalJs(`document.querySelector('.pt-count').textContent`)));
        await clearAllFilters();
        await click(head('vendor') + ' .pt-filter-btn');
        check('the Vendor filter opens a list with a find box', await evalJs(`!!document.querySelector('.pt-pop input[type=text]') && document.querySelectorAll('.pt-pop .pt-opt').length >= 8`));
        await click('.pt-pop .pt-pop-btns .pt-link:last-child');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /^Polymaker/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Vendor: Polymaker only', await st('s.view.length > 0 && s.view.every(function(r){ return r.vendor === "Polymaker"; })'));
        check('a chip names the filter and the funnel is lit', /Vendor: Polymaker/.test(await evalJs(`document.querySelector('.pt-chips').textContent`)) &&
            await evalJs(`document.querySelector('${head('vendor')} .pt-filter-btn').classList.contains('on')`));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        check('Esc closes the filter popover only (no pm_close)', !(await evalJs(`!!document.querySelector('.pt-pop')`)) && !(await sentCmds()).includes('pm_close'));
        await clearAllFilters();
        await click(head('type') + ' .pt-filter-btn');
        await click('.pt-pop .pt-pop-btns .pt-link:last-child');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /^TPU/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Type: TPU only', await st('s.view.length > 0 && s.view.every(function(r){ return r.type === "TPU"; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await clearAllFilters();
        await click(head('source') + ' .pt-filter-btn');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /^System/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Source: User only', await st('s.view.length === s.rows.filter(function(r){ return r.source === "User"; }).length && s.view.every(function(r){ return r.source === "User"; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await clearAllFilters();
        await evalJs(`(function(){ var s = document.querySelector('.pt-toolbar select[data-key="printers"]'); s.value = ${JSON.stringify(PRINTERS[5])}; s.dispatchEvent(new Event('change')); })()`);
        check('Printer drop-down (multi-key): every row shown lists that printer', await st(`s.view.length > 0 && s.view.every(function(r){ return r.printers.indexOf(${JSON.stringify(PRINTERS[5])}) >= 0; }) &&
            s.view.length === s.rows.filter(function(r){ return r.printers.indexOf(${JSON.stringify(PRINTERS[5])}) >= 0; }).length`));
        check('the drop-down names the current printer "(current)" and lists it first',
            await evalJs(`(function(){ var o = document.querySelector('.pt-toolbar select[data-key="printers"]').options; return o[1].textContent === ${JSON.stringify(PRINTERS[0] + ' (current)')}; })()`));
        await clearAllFilters();
        await click(head('nozzle_t') + ' .pt-filter-btn');
        await evalJs(`(function(){ var i = document.querySelector('.pt-pop input[type=number]'); i.value = '240'; i.dispatchEvent(new Event('input')); })()`);
        check('Nozzle at least 240: known values only, all >= 240', await st('s.view.length > 0 && s.view.every(function(r){ return r.nozzle_t !== null && r.nozzle_t >= 240; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await clearAllFilters();
        await click(head('star') + ' .pt-filter-btn');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /Not favourite/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Favourite filter: starred rows only', await st('s.view.length > 0 && s.view.every(function(r){ return r.star; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await clearAllFilters();
        await click(head('checked') + ' .pt-filter-btn');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /Not enabled/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Enabled filter: enabled rows only', await st('s.view.length > 0 && s.view.every(function(r){ return r.checked; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await click('.pt-chips .pt-link');
        check('Clear search and filters brings back all 3000', await st('s.view.length === 3000'));

        // ============================================================ 3. sorting
        await click(head('name') + ' .pt-sort');
        check('Name ascending', await st(`(function(){ var c = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
            for (var i = 1; i < s.view.length; i++) if (c.compare(s.view[i-1].name, s.view[i].name) > 0) return false; return true; })()`));
        await click(head('name') + ' .pt-sort');
        check('Name descending', await st(`(function(){ var c = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
            for (var i = 1; i < s.view.length; i++) if (c.compare(s.view[i-1].name, s.view[i].name) < 0) return false; return true; })()`));
        await click(head('name') + ' .pt-sort');
        check('a third click returns to the original order', await st('s.view.every(function(r, i){ return r.order === i; })'));
        await click(head('nozzle_t') + ' .pt-sort'); await click(head('nozzle_t') + ' .pt-sort');
        check('Nozzle descending, unknown last', await st(`(function(){ var v = s.view.map(function(r){ return r.nozzle_t; }); var k = v.filter(function(x){ return x !== null; });
            for (var i = 1; i < k.length; i++) if (k[i] > k[i-1]) return false; return v.indexOf(null) === k.length; })()`));
        await click(head('nozzle_t') + ' .pt-sort');
        await click(head('mtime') + ' .pt-sort');
        check('Modified (date) ascending, unknown last', await st(`(function(){ var v = s.view.map(function(r){ return r.mtime; }); var k = v.filter(function(x){ return x !== null; });
            for (var i = 1; i < k.length; i++) if (k[i] < k[i-1]) return false; return k.length > 0 && v.indexOf(null) === k.length; })()`));
        await click(head('mtime') + ' .pt-sort'); await click(head('mtime') + ' .pt-sort');

        // ============================================================ 4. locked rows, All / Clear all
        id = await showRow('u|Zed user PLA');
        check('a user preset\'s Enabled box is disabled and says why', await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); var cb = n.querySelector('.pt-td.check input');
            return cb.disabled && cb.checked && /always shown/.test(cb.parentNode.title) && n.classList.contains('locked'); })()`));
        id = await showRow('s|BBL|Bambu PLA Basic');
        await evalJs(`window.__sent = []`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pt-td.check input').click()`);
        await sleep(400);
        check('the row in use cannot be unticked (box disabled, nothing sent)', await st(`s.rows.filter(function(r){ return r.key === 's|BBL|Bambu PLA Basic'; })[0].checked`) &&
            !(await sentCmds()).includes('pm_enabled'));
        await evalJs(`(function(){ var i = document.querySelector('.pt-toolbar .searchTerm'); i.value = 'pla'; i.dispatchEvent(new Event('input')); })()`);
        await evalJs(`window.__sent = []`);
        await evalJs(`document.querySelector('.pt-toolbar .SmallBtn_Green + .SmallBtn').click()`);   // Clear all (after All)
        check('Clear all unticks the rows shown but not the locked ones', await st('s.view.every(function(r){ return r.lock ? r.checked : !r.checked; }) && s.view.some(function(r){ return r.lock; })'));
        await evalJs(`document.querySelector('.pt-toolbar .SmallBtn_Green').click()`);   // All
        check('All ticks the rows shown', await st('s.view.every(function(r){ return r.checked; })'));
        await evalJs(`(function(){ var h = document.querySelector('.pt-hcheck'); h.checked = false; h.dispatchEvent(new Event('change')); })()`);
        check('the header box skips locked rows too', await st('s.view.every(function(r){ return r.lock ? r.checked : !r.checked; })'));
        await sleep(450);
        const batches = await evalJs(`window.__sent.filter(function(m){ return m.command === 'pm_enabled'; })`);
        check('three quick changes go out as one debounced pm_enabled batch', batches.length === 1, batches.length);
        check('the batch holds only real changes, never a locked row', batches.length === 1 && batches[0].changes.length > 0 &&
            batches[0].changes.every(c => !c.key.startsWith('u|') && c.key !== 's|BBL|Bambu PLA Basic' && c.on === false));
        await sleep(200);
        check('count line follows (enabled = locked + the rest left on)', await evalJs(`(function(){ var t = document.querySelector('.pt-count').textContent; var s = PresetManager._state();
            var on = s.rows.filter(function(r){ return r.checked; }).length; return t.indexOf(on + ' enabled') >= 0; })()`));
        await clearAllFilters();

        // ============================================================ 5. focus, keys, refused change
        const sysKey = await st(`s.rows.filter(function(r){ return r.source === 'System' && !r.lock && !r.checked; })[3].key`);
        id = await showRow(sysKey);
        await evalJs(`window.__sent = []`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        check('a row click focuses the row and does not tick it', await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); return n.classList.contains('focus') && document.activeElement === n; })()`) &&
            !(await st(`s.rows[${id}].checked`)));
        await keyOn(id, ' ');
        check('Space ticks the focused row', await st(`s.rows[${id}].checked`));
        await sleep(400);
        check('and sends it at once (pm_enabled on=true)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_enabled' && m.changes.length === 1 && m.changes[0].key === ${JSON.stringify(sysKey)} && m.changes[0].on === true; })`));
        await evalJs(`window.__refuse[${JSON.stringify(sysKey)}] = true`);
        await keyOn(id, ' ');
        await sleep(500);
        check('a change the slicer refuses is undone on the page', await st(`s.rows[${id}].checked === true && s.rows[${id}].enabled === true`));
        const pos0 = await evalJs(`+document.querySelector('.pt-body .pt-row.focus').dataset.pos`);
        await keyOn(id, 'ArrowDown');
        check('ArrowDown moves the focus one row', await evalJs(`+document.querySelector('.pt-body .pt-row.focus').dataset.pos`) === pos0 + 1);
        await evalJs(`document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'PageDown', bubbles: true, cancelable: true }))`);
        check('PageDown moves the focus by a screen', await evalJs(`+document.querySelector('.pt-body .pt-row.focus').dataset.pos`) > pos0 + 5);
        await evalJs(`document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true, cancelable: true }))`);
        check('End goes to the last row', await evalJs(`+document.querySelector('.pt-body .pt-row.focus').dataset.pos`) === 2999);
        await evalJs(`document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true, cancelable: true }))`);
        check('Home goes to the first row', await evalJs(`+document.querySelector('.pt-body .pt-row.focus').dataset.pos`) === 0);
        await evalJs(`document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'F2', bubbles: true, cancelable: true }))`);
        check('F2 on a system row only says it cannot be renamed', /cannot be renamed/.test(await evalJs(`document.getElementById('PmStatus').textContent`)));
        await evalJs(`document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: 'Delete', bubbles: true, cancelable: true }))`);
        check('Delete on a system row only says it cannot be deleted', /cannot be deleted/.test(await evalJs(`document.getElementById('PmStatus').textContent`)) &&
            !(await sentCmds()).includes('pm_delete'));

        // ============================================================ 6. stars
        id = await showRow('s|BBL|Generic PLA');
        await evalJs(`window.__sent = []`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pt-star').click()`);
        check('a star click sends pm_star with the row\'s favourite key', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_star' && m.key === 'Generic PLA'; })`));
        check('and does not tick or focus-toggle the Enabled box', !(await st(`s.rows[${id}].checked`)));
        await sleep(200);
        check('pm_stars lights every row with that key (both vendors\' Generic PLA)', await st(`s.rows.filter(function(r){ return r.fav === 'Generic PLA'; }).every(function(r){ return r.star; })`) &&
            await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pt-star').classList.contains('on')`));
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await keyOn(id, 'f');
        await sleep(200);
        check('F on the focused row toggles the star back (pm_star sent, both rows off)', await evalJs(`window.__sent.filter(function(m){ return m.command === 'pm_star'; }).length === 2`) &&
            await st(`s.rows.filter(function(r){ return r.fav === 'Generic PLA'; }).every(function(r){ return !r.star; })`));

        // ============================================================ 7. re-sent rows keep the view
        await evalJs(`(function(){ var i = document.querySelector('.pt-toolbar .searchTerm'); i.value = 'pla'; i.dispatchEvent(new Event('input')); })()`);
        await click(head('nozzle_t') + ' .pt-sort');
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = 36 * 40; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(150);
        const fkey = await evalJs(`(function(){ var n = document.querySelectorAll('.pt-body .pt-row')[5]; n.children[3].click(); return PresetManager._state().rows[+n.dataset.id].key; })()`);
        const before = await evalJs(`({ top: document.querySelector('.pt-body').scrollTop, n: PresetManager._state().view.length, sort: PresetManager._state().state.sort.key })`);
        await evalJs(`(function(){ var o = JSON.parse(JSON.stringify(window.__mock)); o.command = 'pm_rows'; HandleStudio(o); })()`);
        await sleep(200);
        const after = await evalJs(`({ top: document.querySelector('.pt-body').scrollTop, n: PresetManager._state().view.length, sort: PresetManager._state().state.sort.key,
            search: document.querySelector('.pt-toolbar .searchTerm').value })`);
        check('a re-sent pm_rows keeps search, sort and scroll', after.n === before.n && after.sort === 'nozzle_t' && after.search === 'pla' && Math.abs(after.top - before.top) < 2, JSON.stringify([before, after]));
        check('and the focused row', await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); var n = document.querySelector('.pt-body .pt-row.focus'); return !!f && f.key === ${JSON.stringify(fkey)} && !!n; })()`));
        await clearAllFilters();

        // ============================================================ 7b. rename and delete (phase 2)
        const field = `document.querySelector('.pm-edit-input')`;
        const typeName = async v => { await evalJs(`(function(){ var i = ${field}; i.value = ${JSON.stringify(v)}; i.dispatchEvent(new Event('input', { bubbles: true })); })()`); await sleep(250); };
        const fieldKey = (key, extra) => evalJs(`(function(){ var i = ${field}; var e = new KeyboardEvent('keydown', Object.assign({ key: ${JSON.stringify(key)}, bubbles: true, cancelable: true }, ${JSON.stringify(extra || {})})); i.dispatchEvent(e); return e.defaultPrevented; })()`);
        const statusText = () => evalJs(`document.getElementById('PmStatus').textContent`);
        const rowKeys = () => st(`s.rows.map(function(r){ return r.key; })`);
        await clearAllFilters();
        id = await showRow('u|Fam PLA @' + PRINTERS[0]);
        check('a user row has rename and delete buttons', await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); return !!n.querySelector('.pm-act-rename') && !!n.querySelector('.pm-act-delete'); })()`));
        const sysId = await showRow('s|BBL|Generic PLA');
        check('a system row has none', await evalJs(`!document.querySelector('.pt-body .pt-row[data-id="${sysId}"] .pm-act')`));
        id = await showRow('u|Fam PLA @' + PRINTERS[0]);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await evalJs(`window.__sent = []`);
        await keyOn(id, 'F2');
        await sleep(50);
        check('F2 on a user filament opens the rename field with the name before " @"', await evalJs(`!!${field} && ${field}.value === 'Fam PLA' && document.activeElement === ${field}`));
        check('the printer part stays as fixed text after the field', await evalJs(`document.querySelector('.pm-edit-suffix').textContent === ' @${PRINTERS[0]}'`));
        check('a filament with variants for two printers offers "Only this printer" (off)', await evalJs(`!!document.querySelector('.pm-edit-one') && !document.querySelector('.pm-edit-one').checked`));
        await typeName('Fam @PLA');
        check('typing "@" is invalid at once: the field turns red and says the printer part is kept', await evalJs(`${field}.classList.contains('invalid')`) && /printer part is kept/.test(await statusText()));
        await fieldKey('Enter');
        check('Enter does not rename an invalid name', !(await sentCmds()).includes('pm_rename'));
        await typeName('Fam bad');
        check('a name is checked by the slicer (pm_check_name, whole family)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_check_name' && m.name === 'Fam bad' && m.scope === 'family' && m.id === ${JSON.stringify('u|Fam PLA @' + PRINTERS[0])}; })`));
        check('a name the slicer refuses shows its message and the field turns red', await evalJs(`${field}.classList.contains('invalid')`) && /Name is invalid/.test(await statusText()));
        await fieldKey('Enter');
        check('Enter is blocked for it', !(await sentCmds()).includes('pm_rename'));
        const tickedBefore = await st(`s.rows[${id}].checked`);
        await evalJs(`document.querySelector('.pm-edit-one').click()`);
        await sleep(250);
        check('"Only this printer" re-checks the name for that row only, and does not touch the Enabled box', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_check_name' && m.scope === 'one'; })`) &&
            (await st(`s.rows[${id}].checked`)) === tickedBefore);
        await evalJs(`document.querySelector('.pm-edit-one').click()`);
        // the field survives scrolling far away and back, and a re-sent pm_rows
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = 0; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(120);
        check('scrolled out of view, the row being renamed keeps its field (pinned)', await evalJs(`!!${field} && ${field}.value === 'Fam bad'`));
        await evalJs(`(function(){ var o = JSON.parse(JSON.stringify(window.__mock)); o.command = 'pm_rows'; HandleStudio(o); })()`);
        await sleep(120);
        id = await showRow('u|Fam PLA @' + PRINTERS[0]);
        check('a re-sent pm_rows keeps the field and what was typed', await evalJs(`!!${field} && ${field}.value === 'Fam bad'`));
        await evalJs(`window.__sent = []`);
        const escPrevented = await fieldKey('Escape');
        await sleep(50);
        check('Esc in the field cancels the rename: the name is back, nothing renamed, the dialog stays', escPrevented && !(await evalJs(`!!${field}`)) &&
            await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].textContent === ${JSON.stringify('Fam PLA @' + PRINTERS[0])}`) &&
            !(await sentCmds()).includes('pm_close') && !(await sentCmds()).includes('pm_rename'));
        check('and the row keeps the focus', await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify('u|Fam PLA @' + PRINTERS[0])}; })()`));
        // double-click renames; Enter renames the whole family
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].dispatchEvent(new MouseEvent('dblclick', { bubbles: true }))`);
        await sleep(50);
        check('a double-click on the name opens the field too', await evalJs(`!!${field}`));
        await typeName('Fam PLA+');
        check('a good name is fine (no red)', !(await evalJs(`${field}.classList.contains('invalid')`)));
        await evalJs(`window.__sent = []`);
        await fieldKey('Enter');
        await sleep(250);
        check('Enter sends pm_rename with the typed name and the family scope', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_rename' && m.name === 'Fam PLA+' && m.scope === 'family'; })`));
        const keysAfter = await rowKeys();
        check('both printer variants are renamed, each keeping its printer part', keysAfter.includes('u|Fam PLA+ @' + PRINTERS[0]) && keysAfter.includes('u|Fam PLA+ @' + PRINTERS[1]) &&
            !keysAfter.includes('u|Fam PLA @' + PRINTERS[0]));
        check('the renamed row has the focus, the field is gone', !(await evalJs(`!!${field}`)) && await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify('u|Fam PLA+ @' + PRINTERS[0])}; })()`));
        check('its star went with it', await st(`s.rows.filter(function(r){ return r.key === ${JSON.stringify('u|Fam PLA+ @' + PRINTERS[0])}; })[0].star === true`));
        // the row button, "Only this printer"
        id = await showRow('u|Fam PLA+ @' + PRINTERS[1]);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pm-act-rename').click()`);
        await sleep(50);
        check('the row\'s rename button opens the field', await evalJs(`!!${field} && ${field}.value === 'Fam PLA+'`));
        await evalJs(`document.querySelector('.pm-edit-one').click()`);
        await typeName('Solo PLA');
        await evalJs(`window.__sent = []`);
        await fieldKey('Enter');
        await sleep(250);
        const keysOne = await rowKeys();
        check('"Only this printer" renames that variant alone (scope one)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_rename' && m.scope === 'one'; })`) &&
            keysOne.includes('u|Solo PLA @' + PRINTERS[1]) && keysOne.includes('u|Fam PLA+ @' + PRINTERS[0]));
        // Tab renames and moves on to the next row's name (user rows only, by name: the next is Zed user PLA)
        await click(head('name') + ' .pt-sort');
        await evalJs(`(function(){ var f = PresetManager.table.setFilter('source'); f['System'] = 1; PresetManager.table.recompute(false); })()`);
        id = await showRow('u|Solo PLA @' + PRINTERS[1]);
        await keyOn(id, 'F2');
        await sleep(50);
        await typeName('Solo PLA 2');
        const nextKey = await st(`(function(){ var v = s.view; for (var i = 0; i < v.length; i++) if (v[i].key === ${JSON.stringify('u|Solo PLA @' + PRINTERS[1])}) return v[i + 1].key; })()`);
        await fieldKey('Tab');
        await sleep(300);
        check('Tab renames and opens the next row\'s name', (await rowKeys()).includes('u|Solo PLA 2 @' + PRINTERS[1]) &&
            await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify(nextKey)}; })()`) &&
            nextKey === 'u|Zed user PLA' && await evalJs(`!!${field} && ${field}.value === 'Zed user PLA'`), nextKey);
        await docKey('Escape');
        await click(head('name') + ' .pt-sort');
        await click(head('name') + ' .pt-sort');
        check('Esc closes that field first (no pm_close)', !(await evalJs(`!!${field}`)) && !(await sentCmds()).includes('pm_close'));
        // delete (a generated user filament; Zed user PLA is kept for Edit printers... below)
        const delKey = await st(`s.rows.filter(function(r){ return r.source === 'User' && /^My /.test(r.name); })[3].key`);
        id = await showRow(delKey);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        const afterZed = await st(`(function(){ var v = s.view; for (var i = 0; i < v.length; i++) if (v[i].key === ${JSON.stringify(delKey)}) return (v[i + 1] || v[i - 1]).key; })()`);
        await evalJs(`window.__sent = []; window.__confirmDelete = false`);
        await keyOn(id, 'Delete');
        await sleep(200);
        check('Delete on a user row sends pm_delete (the slicer asks to confirm)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_delete' && m.id === ${JSON.stringify(delKey)}; })`));
        check('a cancelled delete keeps the row and its focus', (await rowKeys()).includes(delKey) &&
            await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify(delKey)}; })()`));
        await evalJs(`window.__confirmDelete = true`);
        id = await showRow(delKey);
        await keyOn(id, 'Delete');
        await sleep(250);
        check('a confirmed delete removes the row; the next row in view takes the focus', !(await rowKeys()).includes(delKey) &&
            await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify(afterZed)}; })()`), afterZed);
        id = await showRow('u|Solo PLA 2 @' + PRINTERS[1]);
        await evalJs(`window.__sent = []`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pm-act-delete').click()`);
        await sleep(250);
        check('the row\'s delete button deletes too', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_delete'; })`) && !(await rowKeys()).includes('u|Solo PLA 2 @' + PRINTERS[1]));
        // a click off the field renames, as Enter does (owner hand test of #909)
        await clearAllFilters();
        const blurKey = await st(`s.rows.filter(function(r){ return r.source === 'User' && /^My /.test(r.name); })[5].key`);
        const otherKey = await st(`s.rows.filter(function(r){ return r.source === 'User' && /^My /.test(r.name); })[6].key`);
        id = await showRow(blurKey);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await keyOn(id, 'F2');
        await sleep(50);
        const blurBase = await evalJs(`${field}.value`);
        await typeName(blurBase + ' blur');
        await evalJs(`window.__sent = []`);
        const otherId = await st(`s.rows.filter(function(r){ return r.key === ${JSON.stringify(otherKey)}; })[0].id`);
        await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${otherId}"]').children[3];
            n.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })); n.click(); })()`);
        await sleep(300);
        check('a click on another row renames the row being edited (pm_rename, as Enter)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_rename' && m.id === ${JSON.stringify(blurKey)} && m.name === ${JSON.stringify(blurBase + ' blur')}; })`) &&
            !(await evalJs(`!!${field}`)) && (await rowKeys()).some(function (k) { return k.indexOf('u|' + blurBase + ' blur') === 0; }));
        check('and the row clicked keeps the focus', await evalJs(`(function(){ var f = PresetManager.table.focusedRow(); return !!f && f.key === ${JSON.stringify(otherKey)}; })()`));
        // an invalid name: a click on blank space keeps the field open and red, nothing renamed
        id = await showRow(otherKey);
        await keyOn(id, 'F2');
        await sleep(50);
        await typeName('Still bad');
        await evalJs(`window.__sent = []`);
        await evalJs(`(function(){ var f = document.getElementById('PmFooter'); f.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })); document.querySelector('.pm-edit-input').blur(); f.click(); })()`);
        await sleep(300);
        check('an invalid name stays in its red field when the field loses the focus (nothing renamed)', !(await sentCmds()).includes('pm_rename') &&
            await evalJs(`!!${field} && ${field}.classList.contains('invalid') && ${field}.value === 'Still bad'`) && /Name is invalid/.test(await statusText()));
        // ticking "Only this printer" is inside the field's box: no rename
        await docKey('Escape');
        await sleep(50);
        check('Esc still cancels that field', !(await evalJs(`!!${field}`)) && !(await sentCmds()).includes('pm_rename'));
        id = await showRow('u|Fam PLA+ @' + PRINTERS[0]);
        await keyOn(id, 'F2');
        await sleep(50);
        await evalJs(`window.__sent = []`);
        await evalJs(`(function(){ var c = document.querySelector('.pm-edit-one'); if (!c) return; c.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })); c.focus(); c.click(); })()`);
        await sleep(300);
        check('a click on "Only this printer" does not rename (the field stays open)', !(await sentCmds()).includes('pm_rename') && await evalJs(`!!${field}`));
        await docKey('Escape');
        // no browser context menu
        const ctxRow = await evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row'); var e = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, button: 2 }); n.dispatchEvent(e); return e.defaultPrevented; })()`);
        const ctxFoot = await evalJs(`(function(){ var e = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, button: 2 }); document.getElementById('PmFooter').dispatchEvent(e); return e.defaultPrevented; })()`);
        check('right-click on a row or the footer: no browser context menu (filament page)', ctxRow === true && ctxFoot === true);
        check('no uncaught exception while renaming and deleting', errors.length === 0, errors.join(' | '));
        await clearAllFilters();

        // ============================================================ 8. buttons, Esc, Ctrl+F
        await evalJs(`window.__sent = []`);
        id = await showRow(await st(`s.rows.filter(function(r){ return r.source === 'System'; })[0].key`));
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await click('#PmEdit');
        check('Edit printers... on a system row sends nothing', !(await sentCmds()).includes('pm_edit'));
        id = await showRow('u|Zed user PLA');
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await click('#PmEdit');
        check('Edit printers... on a user filament sends pm_edit with its filament_id', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_edit' && m.filament_id === 'PZED'; })`));
        await click('#PmCreate');
        check('Create new sends pm_create', (await sentCmds()).includes('pm_create'));
        await docKey('f', { ctrlKey: true });
        check('Ctrl+F puts the focus in the search box', await evalJs(`document.activeElement === document.querySelector('.pt-toolbar .searchTerm')`));
        const prevented = await evalJs(`(function(){ var e = new KeyboardEvent('keydown', { key: 'a', bubbles: true, cancelable: true }); document.querySelector('.pt-toolbar .searchTerm').dispatchEvent(e); return e.defaultPrevented; })()`);
        check('typing in the search box is not eaten', prevented === false);
        // a pending tick, then Esc: the tick goes out before pm_close
        await evalJs(`window.__sent = []`);
        const k2 = await st(`s.rows.filter(function(r){ return r.source === 'System' && !r.lock && !r.checked; })[7].key`);
        id = await showRow(k2);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pt-td.check input').click()`);
        await click(head('type') + ' .pt-filter-btn');
        await docKey('Escape');
        check('Esc with a popover open: nothing sent yet (the popover closes first)', !(await sentCmds()).includes('pm_close'));
        await evalJs(`document.querySelector('.pt-pop') && document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await docKey('Escape');
        check('Esc with nothing open: the pending tick is flushed, then pm_close', (await sentCmds()) === 'pm_enabled,pm_close', await sentCmds());
        await evalJs(`window.__sent = []`);
        await evalJs(`HandleStudio({ command: 'pm_escape' })`);
        check('pm_escape from the slicer does the same (pm_close)', (await sentCmds()) === 'pm_close');
        await evalJs(`window.__sent = []`);
        await click('#CloseBtn');
        check('Close sends pm_close', (await sentCmds()) === 'pm_close');
        check('no uncaught exception on the filament page', errors.length === 0, errors.join(' | '));
        await shot('filament_light.png');

        // ============================================================ 9. data_table defaults unchanged for other pages
        check('the engine\'s opt-ins are off by default (Printer Selection still ticks on a row click)',
            await evalJs(`(function(){ var t = DataTable.create({ cols: [{ key: 'checked', kind: 'bool', cls: 'check' }, { key: 'name', kind: 'text' }] });
                var d = document.createElement('div'); document.body.appendChild(d); t.init(d); t.setRows([{ id: 0, order: 0, name: 'x', checked: false, hay: 'x' }]);
                d.querySelector('.pt-body .pt-row').children[1].click(); var on = t.rows()[0].checked; d.remove(); return on; })()`));

        // ============================================================ 10. dark mode
        const bgLight = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        await load('filament', { dark: true });
        const bgDark = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        check('dark user agent: the table turns dark', bgLight === 'rgb(255, 255, 255)' && bgDark === 'rgb(45, 45, 49)', bgLight + ' / ' + bgDark);
        check('dark: the star and focus tokens switch too', (await evalJs(`getComputedStyle(document.documentElement).getPropertyValue('--pt-star-on').trim()`)).toUpperCase() === '#F5B82E' &&
            (await evalJs(`getComputedStyle(document.documentElement).getPropertyValue('--pt-focus-bg').trim()`)).toUpperCase() === '#2F4744');
        await shot('filament_dark.png');

        // ============================================================ 10b. macOS keys
        await load('filament', { mac: true });
        id = await showRow('u|Zed user PLA');
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[3].click()`);
        await keyOn(id, 'Enter');
        await sleep(50);
        check('macOS: Return on a user row opens the rename field', await evalJs(`!!document.querySelector('.pm-edit-input')`));
        await evalJs(`(function(){ var i = document.querySelector('.pm-edit-input'); i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })); })()`);
        await evalJs(`window.__sent = []; window.__confirmDelete = false`);
        id = await showRow('u|Zed user PLA');
        await keyOn(id, 'Backspace', { metaKey: true });
        await sleep(100);
        check('macOS: Cmd+Backspace deletes (pm_delete)', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_delete' && m.id === 'u|Zed user PLA'; })`));
        check('no uncaught exception with the macOS keys', errors.length === 0, errors.join(' | '));

        // ============================================================ 11. Process manager
        await load('process');
        check('process page loads with no uncaught exception', errors.length === 0, errors.join(' | '));
        check('process columns: Enabled, Favourite, Name, Printer, Nozzle, Layer height, Quality / parent, Source, Modified, Actions',
            await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-head .pt-th'), function(t){ return t.dataset.key; }).join(',')`) === 'checked,star,name,printers,nozzles,lh,tier,source,mtime,actions');
        check('it starts on the current printer only', await st(`s.rows.length === 600 && s.view.length === 50 && s.view.every(function(r){ return r.printers.indexOf(${JSON.stringify(PRINTERS[0])}) >= 0; })`), await st('s.view.length'));
        check('the Printer drop-down shows the current printer', await evalJs(`document.querySelector('.pt-toolbar select[data-key="printers"]').value === ${JSON.stringify(PRINTERS[0])}`));
        await evalJs(`(function(){ var s = document.querySelector('.pt-toolbar select[data-key="printers"]'); s.value = ${JSON.stringify(PRINTERS[3])}; s.dispatchEvent(new Event('change')); })()`);
        check('another printer can be picked', await st(`s.view.length === 50 && s.view.every(function(r){ return r.printers[0] === ${JSON.stringify(PRINTERS[3])}; })`));
        await evalJs(`(function(){ var s = document.querySelector('.pt-toolbar select[data-key="printers"]'); s.value = ''; s.dispatchEvent(new Event('change')); })()`);
        check('"All printers" shows all 600', await st('s.view.length === 600'));
        check('quality tiers and Detached are listed', await st(`['Standard','Fine','Extra Fine','Draft','Strength','Detached'].every(function(t){ return s.rows.some(function(r){ return r.tier === t; }); })`));
        await click(head('lh') + ' .pt-sort');
        check('Layer height sorts as a number', await st(`(function(){ for (var i = 1; i < s.view.length; i++) if (s.view[i].lh < s.view[i-1].lh) return false; return true; })()`));
        await evalJs(`window.__sent = []`);
        const pkey = await st(`s.rows.filter(function(r){ return r.source === 'System' && !r.lock; })[2].key`);
        id = await showRow(pkey);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"] .pt-td.check input').click()`);
        await sleep(450);
        check('unticking a system process sends pm_enabled on=false', await evalJs(`window.__sent.some(function(m){ return m.command === 'pm_enabled' && m.changes[0].key === ${JSON.stringify(pkey)} && m.changes[0].on === false; })`));
        const ukey = await st(`s.rows.filter(function(r){ return r.source === 'User'; })[0].key`);
        id = await showRow(ukey);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${id}"]').children[2].click()`);
        await keyOn(id, 'F2');
        await sleep(50);
        check('a user process is renamed whole (no fixed printer part, no "Only this printer")', await evalJs(`(function(){ var i = document.querySelector('.pm-edit-input');
            return !!i && i.value === PresetManager._state().rows[${id}].name && !document.querySelector('.pm-edit-suffix') && !document.querySelector('.pm-edit-one'); })()`));
        await docKey('Escape');
        check('right-click: no browser context menu (process page)', await evalJs(`(function(){ var e = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, button: 2 }); document.querySelector('.pt-body .pt-row').dispatchEvent(e); return e.defaultPrevented; })()`) === true);
        check('no uncaught exception on the process page', errors.length === 0, errors.join(' | '));
        await shot('process_light.png');
    } finally {
        try { br.kill(); } catch (e) { }
        server.close();
    }
    console.log(`\n${passed} passed, ${failures} failed`);
    return failures ? 1 : 0;
}

main().then(c => process.exit(c), e => { console.error(e); process.exit(2); });

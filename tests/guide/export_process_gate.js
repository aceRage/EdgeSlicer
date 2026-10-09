// Gate: the Process presets table of Export Preset Bundle (resources/web/guide/export_process).
//
// Same harness as printer_table_gate.js: a loopback-only Node http server serves the shipped
// resources/ folder; a headless Edge / Chrome loads the page over the DevTools protocol (Node 22's
// built-in WebSocket). window.wx is a stub that records what the page sends and answers
// "export_table_ready" with mock rows shaped the way CreatePresetsDialog.cpp sends them. No
// slicer, no printer, nothing on the desktop.
//
// It checks:
//   * the page loads with no exception and asks for the rows; 600 rows render as a few dozen DOM rows
//   * the columns (select, preset, printer, inherits from, layer height, last modified) and their text:
//     "All printers", "first +N", dashes for unknown values, a readable date
//   * the printer drop-down ("All printers" by default) filters; the search box covers name and printer
//   * the header checkbox and All / Clear all act on the rows shown only; ticks survive a filter
//   * presets of two printers can be ticked one by one; the slicer is told exactly the ticked ids
//   * sorting (preset, printer, layer height numerically, last modified), range filter on layer height,
//     the Printer funnel keeps the drop-down in step
//   * Esc closes an open filter first, then asks the slicer to close the dialog
//   * an empty list says so; dark mode (the slicer's "dark" user agent)
//
// Run: node tests/guide/export_process_gate.js      (skips, exit 0, when no Chromium is found)
// SHOTS=<dir> also writes screenshots (light, dark, filtered, popover).
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

// ---- mock rows: 600 user process presets over 12 printers -----------------------------------------
const PRINTERS = [];
const NATO = ['Alpha', 'Bravo', 'Charlie', 'Delta', 'Echo', 'Foxtrot', 'Golf', 'Hotel', 'India', 'Juliet', 'Kilo', 'Lima'];
for (let i = 0; i < 12; i++) PRINTERS.push('Snap ' + NATO[i] + ' (0.4 nozzle)');
PRINTERS.sort();
function synthetic() {
    const rows = [];
    const parents = ['0.20mm Standard @Base', '0.16mm Optimal @Base', '0.28mm Draft @Base', ''];
    for (let n = 0; n < 600; n++) {
        let printers, all = false;
        if (n % 40 === 7) { printers = PRINTERS.slice(); all = true; }               // compatible with everything
        else if (n % 9 === 3) printers = [PRINTERS[n % 12], PRINTERS[(n + 5) % 12]].sort(); // two printers
        else printers = [PRINTERS[n % 12]];
        rows.push({
            id: n, name: 'Preset ' + String(n).padStart(3, '0') + (n % 13 === 0 ? ' fast' : ''), printers, all,
            inherits: parents[n % 4], lh: n % 31 === 5 ? null : [0.2, 0.16, 0.28, 0.125][n % 4],
            mtime: n % 29 === 4 ? null : 1790000000 + n * 86400,
        });
    }
    return { rows, printers: PRINTERS.slice() };
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


function stub(mock) {
    return `window.__sent = []; window.__mock = ${JSON.stringify(mock)};
        window.wx = { postMessage: function (s) { var m = JSON.parse(s); window.__sent.push(m);
            if (m.command === 'export_table_ready') setTimeout(function () {
                HandleStudio({ command: 'response_process_presets', response: window.__mock }); }, 20); } };`;
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const origin = 'http://127.0.0.1:' + server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'exptable-'));
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
        let stubId = null;
        const load = async ({ dark = false, mock = synthetic(), w = 1000, h = 560, wait = true } = {}) => {
            await S('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
            await S('Emulation.setUserAgentOverride', { userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36 SM-Slicer/2.4' + (dark ? ' dark' : '') });
            if (stubId) await S('Page.removeScriptToEvaluateOnNewDocument', { identifier: stubId });
            stubId = (await S('Page.addScriptToEvaluateOnNewDocument', { source: stub(mock) })).result.identifier;
            errors.length = 0;
            await S('Page.navigate', { url: origin + '/web/guide/export_process/index.html?lang=en' });
            for (let i = 0; i < 50; i++) { await sleep(100); try { if (await evalJs('!!(window.ProcessExport && ProcessExport._state().rows.length)') || (!wait)) break; } catch (e) { } }
            await sleep(300);
        };
        const shot = async name => {
            if (!SHOTS) return;
            fs.mkdirSync(SHOTS, { recursive: true });
            const r = await S('Page.captureScreenshot', { format: 'png' });
            fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64'));
            console.log('     shot ' + path.join(SHOTS, name));
        };
        const st = expr => evalJs(`(function(){ var s = ProcessExport._state(); return (${expr}); })()`);
        const click = sel => evalJs(`(function(){ var e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`);
        const head = key => `.pt-th[data-key="${key}"]`;
        const text = sel => evalJs(`(document.querySelector(${JSON.stringify(sel)}) || {}).textContent`);
        const setVal = (sel, v) => evalJs(`(function(){ var i = document.querySelector(${JSON.stringify(sel)}); i.value = ${JSON.stringify(v)}; i.dispatchEvent(new Event(i.tagName === 'SELECT' ? 'change' : 'input')); })()`);
        const lastSel = () => evalJs(`(function(){ var m = window.__sent.filter(function(m){ return m.command === 'export_table_selection'; }); return m.length ? m[m.length - 1] : null; })()`);
        const esc = () => evalJs(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        const P_A = PRINTERS[0], P_B = PRINTERS[1];

        // ============================================================ 1. load
        await load();
        check('the page loads with no uncaught exception', errors.length === 0, errors.join(' | '));
        check('it asks the slicer for the rows (export_table_ready)', await evalJs(`window.__sent[0].command === 'export_table_ready'`));
        check('600 rows loaded, none ticked', await st('s.rows.length === 600 && s.rows.every(function(r){ return !r.checked; })'));
        check('count line: 600 presets, 0 selected', /600 presets .* 0 selected/.test(await text('.pt-count')), await text('.pt-count'));
        const domRows = await evalJs(`document.querySelectorAll('.pt-body .pt-row').length`);
        check('virtualised: a few dozen DOM rows', domRows > 5 && domRows < 60, domRows);
        check('columns: select, preset, printer, inherits from, layer height, last modified',
            await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-head .pt-th'), function(t){ return t.dataset.key; }).join(',')`) === 'checked,name,printers,inherits,lh,mtime');
        check('header texts', await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-head .pt-sort .lbl'), function(t){ return t.textContent; }).join('|')`) === 'Export|Preset|Printer|Inherits from|Layer height|Last modified');
        check('the printer drop-down starts on "All printers" and lists every printer',
            await evalJs(`(function(){ var s = document.querySelector('.pt-toolbar select'); return s.value === '' && s.options.length === 13 && s.options[0].textContent === 'All printers'; })()`));

        // ---- cell text
        const cells = n => evalJs(`(function(){ var r = document.querySelector('.pt-body .pt-row[data-id="${n}"]'); return r ? Array.prototype.map.call(r.children, function(c){ return c.textContent; }) : null; })()`);
        const first = await cells(0);
        check('row 0: name, printer, inherits, layer height', first && first[1] === 'Preset 000 fast' && first[2] === PRINTERS[0] && first[3] === '0.20mm Standard @Base' && first[4] === '0.20', JSON.stringify(first));
        check('last modified is a readable date, not "Invalid"', first[5] && !/Invalid|NaN/.test(first[5]) && first[5] !== '\u2014', first[5]);
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = 7 * 36 - 10; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(150);
        const all7 = await cells(7);
        check('a preset compatible with everything says "All printers"', all7 && all7[2] === 'All printers', JSON.stringify(all7));
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = 3 * 36 - 10; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(150);
        const two = await cells(3);
        check('two printers show the first and +1', two && /^Snap .* \+1$/.test(two[2]), JSON.stringify(two));
        check('0.125 mm is shown as 0.125', two && two[4] === '0.125', JSON.stringify(two));
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = 5 * 36 - 10; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(150);
        const nul = await cells(5);
        check('unknown layer height shows a dash, an unknown date too', nul && nul[4] === '—' && nul[5] !== '—' && await cells(4).then(c => c && c[5] === '—'), JSON.stringify(nul));
        await evalJs(`document.querySelector('.pt-body').scrollTop = 0`);

        // ============================================================ 2. printer drop-down, search
        await setVal('.pt-toolbar select', P_A);
        const inA = await st(`s.rows.filter(function(r){ return r.printers.indexOf(${JSON.stringify(P_A)}) >= 0; }).length`);
        check('picking a printer shows only presets listed under it', await st('s.view.length') === inA && inA > 0 && inA < 600, inA);
        check('and the count line says so', new RegExp(inA + ' of 600 shown').test(await text('.pt-count')), await text('.pt-count'));
        check('a chip names the printer filter', /Printer: /.test(await text('.pt-chips')), await text('.pt-chips'));
        check('presets compatible with everything stay in the list', await st('s.view.some(function(r){ return r.all; })'));
        await setVal('.pt-toolbar select', '');
        check('"All printers" brings back all 600', await st('s.view.length === 600'));

        await setVal('.pt-toolbar .searchTerm', 'fast');
        check('search by preset name', await st('s.view.length > 0 && s.view.length < 600 && s.view.every(function(r){ return r.name.indexOf("fast") >= 0; })'), await st('s.view.length'));
        await setVal('.pt-toolbar .searchTerm', 'bravo');
        check('search by printer name', await st(`s.view.length > 0 && s.view.every(function(r){ return r.printers.join(" ").toLowerCase().indexOf("bravo") >= 0; })`), await st('s.view.length'));
        await setVal('.pt-toolbar .searchTerm', 'fast charlie');
        check('search words may mix name and printer', await st(`s.view.length > 0 && s.view.every(function(r){ return /fast/.test(r.name) && r.printers.join(" ").indexOf("Charlie") >= 0; })`), await st('s.view.length'));
        await click('.pt-chips .pt-link');
        check('Clear search and filters restores everything', await st('s.view.length === 600 && s.state.search === ""'));

        // ============================================================ 3. selection: header checkbox, All / Clear, filters
        await evalJs(`window.__sent = []`);
        await setVal('.pt-toolbar select', P_A);
        await click('.pt-head .pt-hcheck');
        const nA = inA;
        check('header checkbox ticks the rows shown', await st(`s.rows.filter(function(r){ return r.checked; }).length`) === nA);
        check('and says "N selected"', new RegExp(nA + ' selected').test(await text('.pt-count')), await text('.pt-count'));
        check('the slicer is told exactly those ids', JSON.stringify((await lastSel()).ids.slice().sort((a, b) => a - b)) ===
            JSON.stringify(await st(`s.rows.filter(function(r){ return r.checked; }).map(function(r){ return r.id; }).sort(function(a, b){ return a - b; })`)) && (await lastSel()).count === nA);
        check('header checkbox is fully ticked', await evalJs(`(function(){ var c = document.querySelector('.pt-hcheck'); return c.checked && !c.indeterminate; })()`));
        await setVal('.pt-toolbar select', P_B);
        check('ticks stay when a filter hides the rows', await st(`s.rows.filter(function(r){ return r.checked; }).length`) === nA);
        check('the count line still counts them', new RegExp(nA + ' selected').test(await text('.pt-count')), await text('.pt-count'));
        const bOnly = await st(`s.view.filter(function(r){ return r.checked; }).length`);
        check('header checkbox is partly ticked when only some shown rows are ticked (the shared ones)', await evalJs(`(function(){ var c = document.querySelector('.pt-hcheck'); return ${bOnly} === 0 ? !c.checked : c.indeterminate; })()`));
        await click('.pt-toolbar .SmallBtn');   // Clear all, rows shown only
        const left = await st(`s.rows.filter(function(r){ return r.checked; }).length`);
        check('Clear all clears the rows shown (printer B) only', left === nA - bOnly, left + ' vs ' + (nA - bOnly));
        await click('.pt-toolbar .SmallBtn_Green');   // All
        check('All ticks the rows shown (printer B)', await st(`s.view.every(function(r){ return r.checked; })`));
        await setVal('.pt-toolbar select', '');
        await click('.pt-toolbar .SmallBtn');   // Clear all with nothing filtered = everything
        check('Clear all with no filter clears every preset', await st('s.rows.every(function(r){ return !r.checked; })') && (await lastSel()).count === 0);

        // ---- individual presets across two printers
        await evalJs(`window.__sent = []`);
        await setVal('.pt-toolbar select', P_A);
        const a1 = await st(`s.view.filter(function(r){ return !r.all && r.printers.length === 1; })[0].id`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${a1}"] input').click()`);
        await setVal('.pt-toolbar select', P_B);
        const b1 = await st(`s.view.filter(function(r){ return !r.all && r.printers.length === 1; })[2].id`);
        await evalJs(`document.querySelector('.pt-body .pt-row[data-id="${b1}"]').children[1].click()`);   // a click on the row, not the box
        check('one preset of printer A and one of printer B are ticked', JSON.stringify((await lastSel()).ids.sort((x, y) => x - y)) === JSON.stringify([a1, b1].sort((x, y) => x - y)), JSON.stringify(await lastSel()));
        await setVal('.pt-toolbar select', '');
        check('with all printers shown, exactly those two are ticked', await st(`s.rows.filter(function(r){ return r.checked; }).length`) === 2 && /2 selected/.test(await text('.pt-count')), await text('.pt-count'));
        await click(head('checked') + ' .pt-filter-btn');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /Not selected/.test(l.textContent); })[0].querySelector('input').click()`);
        check('the Export column filters to the selected presets', await st('s.view.length === 2'), await st('s.view.length'));
        await esc();
        await click('.pt-chips .pt-link');

        // ============================================================ 4. sorting
        await click(head('name') + ' .pt-sort');
        check('sort by preset name ascending', await st('s.view[0].name === "Preset 000 fast" && s.view[1].name === "Preset 001"'));
        await click(head('name') + ' .pt-sort');
        check('and descending', await st('s.view[0].name === "Preset 599"'));
        await click(head('name') + ' .pt-sort');
        check('a third click restores the original order', await st('s.view[0].id === 0 && s.view[599].id === 599 && s.state.sort.key === null'));
        await click(head('lh') + ' .pt-sort');
        check('layer height sorts numerically, unknown last', await st('s.view[0].lh === 0.125 && s.view[s.view.length - 1].lh === null'));
        await click(head('lh') + ' .pt-sort');
        check('layer height descending, unknown still last', await st('s.view[0].lh === 0.28 && s.view[s.view.length - 1].lh === null'));
        await click(head('mtime') + ' .pt-sort');
        check('last modified sorts by time', await st('(function(){ var v = s.view.filter(function(r){ return r.mtime !== null; }); for (var i = 1; i < v.length; i++) if (v[i].mtime < v[i-1].mtime) return false; return true; })()'));
        await click(head('printers') + ' .pt-sort');
        check('printer sorts by name', await st('(function(){ var v = s.view.filter(function(r){ return !r.all; }); for (var i = 1; i < v.length; i++) if (v[i].printers[0] < v[i - 1].printers[0]) return false; return true; })()'));
        await click(head('printers') + ' .pt-sort'); await click(head('printers') + ' .pt-sort');

        // ============================================================ 5. filters: layer height range, Printer funnel
        await click(head('lh') + ' .pt-filter-btn');
        await evalJs(`(function(){ var i = document.querySelector('.pt-pop input[type=number]'); i.value = '0.2'; i.dispatchEvent(new Event('input')); })()`);
        check('layer height at least 0.2', await st('s.view.length > 0 && s.view.every(function(r){ return r.lh !== null && r.lh >= 0.2; })'));
        check('chip "Layer height ≥ 0.20"', /Layer height \u2265 0\.20/.test(await text('.pt-chips')), await text('.pt-chips'));
        await esc();
        await click('.pt-chips .pt-link');

        await click(head('printers') + ' .pt-filter-btn');
        check('the Printer funnel lists the printers with a find box', await evalJs(`!!document.querySelector('.pt-pop input[type=text]') && document.querySelectorAll('.pt-pop .pt-opt').length === 12`));
        await click('.pt-pop .pt-pop-btns .pt-link:last-child');   // Select none
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return l.textContent.indexOf("Snap Charlie") === 0; })[0].querySelector('input').click()`);
        check('ticking one printer in the funnel shows its presets', await st(`s.view.length > 0 && s.view.every(function(r){ return r.printers.some(function(p){ return p.indexOf("Snap Charlie") === 0; }); })`));
        check('and the drop-down follows', await evalJs(`document.querySelector('.pt-toolbar select').value.indexOf('Snap Charlie') === 0`));
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return l.textContent.indexOf("Snap Delta") === 0; })[0].querySelector('input').click()`);
        check('two printers in the funnel: the drop-down says "2 printers"', await evalJs(`document.querySelector('.pt-toolbar select').selectedOptions[0].textContent`) === '2 printers', await evalJs(`document.querySelector('.pt-toolbar select').selectedOptions[0].textContent + ' / ' + document.querySelector('.pt-chips').textContent`));
        await esc();
        await click('.pt-chips .pt-link');
        check('Clear search and filters sets the drop-down back to "All printers"', await evalJs(`document.querySelector('.pt-toolbar select').value`) === '');

        await click(head('inherits') + ' .pt-filter-btn');
        check('the Inherits from funnel lists the parents and a dash for none', await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-pop .pt-opt span:not(.n)'), function(s){ return s.textContent; }).indexOf('\u2014') >= 0`));
        await esc();

        // ============================================================ 6. Esc
        await evalJs(`window.__sent = []`);
        await click(head('lh') + ' .pt-filter-btn');
        await esc();
        check('Esc closes an open filter, not the dialog', !(await evalJs(`!!document.querySelector('.pt-pop')`)) && !(await evalJs(`window.__sent.some(function(m){ return m.command === 'export_table_cancel'; })`)));
        await esc();
        check('Esc with nothing open asks the slicer to close the dialog', await evalJs(`window.__sent.some(function(m){ return m.command === 'export_table_cancel'; })`));
        check('no uncaught exception during all that', errors.length === 0, errors.join(' | '));

        // ============================================================ 7. a reloaded page gets its ticks back; empty list; themes
        await load({ mock: Object.assign(synthetic(), { selected: [2, 5, 9] }) });
        check('ids the slicer already knows come back ticked (the page was reloaded)', await st('s.rows.filter(function(r){ return r.checked; }).map(function(r){ return r.sourceId; }).join(",") === "2,5,9"') &&
            /3 selected/.test(await text('.pt-count')) && JSON.stringify((await lastSel()).ids) === '[2,5,9]', await text('.pt-count'));
        await load({ mock: { rows: [], printers: [] }, wait: false });
        await sleep(300);
        check('an empty list says there is nothing to export', /no user process presets/i.test(await text('.pt-empty')) && await evalJs(`getComputedStyle(document.querySelector('.pt-empty')).display`) === 'block', await text('.pt-empty'));
        check('and the header checkbox is disabled', await evalJs(`document.querySelector('.pt-hcheck').disabled`));

        await load();
        const bgLight = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        const selLight = await evalJs(`getComputedStyle(document.querySelector('.pt-toolbar select')).backgroundColor`);
        await shot('export_light.png');
        await load({ dark: true });
        const bgDark = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        const selDark = await evalJs(`getComputedStyle(document.querySelector('.pt-toolbar select')).backgroundColor`);
        check('dark user agent: the table and the drop-down turn dark', bgLight === 'rgb(255, 255, 255)' && bgDark === 'rgb(45, 45, 49)' && selLight === 'rgb(255, 255, 255)' && selDark === 'rgb(45, 45, 49)',
            [bgLight, bgDark, selLight, selDark].join(' / '));
        check('dark: body text is light', await evalJs(`getComputedStyle(document.querySelector('.pt-body .pt-td:nth-child(2)')).color`) === 'rgb(239, 239, 240)');
        await shot('export_dark.png');

        if (SHOTS) {
            await load();
            await setVal('.pt-toolbar select', P_A);
            for (const n of [0, 1, 2]) await evalJs(`(function(){ var r = ProcessExport._state().view[${n}]; document.querySelector('.pt-body .pt-row[data-id="' + r.id + '"] input').click(); })()`);
            await sleep(300);
            await shot('export_filtered_selected.png');
            await click(head('printers') + ' .pt-filter-btn');
            await sleep(300);
            await shot('export_printer_popover.png');
        }
    } finally {
        try { br.kill(); } catch (e) { }
        server.close();
    }
    console.log(`\n${passed} passed, ${failures} failed`);
    return failures ? 1 : 0;
}

main().then(c => process.exit(c), e => { console.error(e); process.exit(2); });

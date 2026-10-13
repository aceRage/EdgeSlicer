// Gate: the Home tab's Vaults section (resources/web/home: index.html, home.js, home.css).
//
// A plain Node http server (loopback, no dependencies) serves the real shipped page. Before the page
// runs, a stand-in for the slicer is injected as window.wx - the script channel HomePanel.cpp listens
// on - and records every message; the test answers through window.HomeApp.receive() the way
// HomePanel does. A headless Edge / Chrome is driven over the DevTools protocol (Node 22's built-in
// WebSocket). No slicer, no network beyond loopback, nothing touches the desktop: the "keyboard" is
// Input.dispatchKeyEvent into the headless page.
//
// It checks: the page loads with no uncaught exception; the rail is Recent, Library, Vendors, Vaults,
// Print History; Vendors is untouched (its own section, Import and Add connector, a show_section
// "vendors" still opens it); a saved section "vaults" opens Vaults; the empty state and its Add vault
// button; the Add vault chooser (focus on the first choice, Folder or network share sends
// library_add_folder and closes, Escape / Cancel / the backdrop close it, no vendor connector in it);
// the folders card counts folders and files, names an unreachable share, and leads to the Library and
// its folder manager; text contrast of what Vaults adds >= 4.5:1 in both themes.
//
// Run: node tests/phone/home_vaults_gate.js     (skips, exit 0, when no Chromium is found)
// WEB_DIR=<dir> points it at another copy of the page. SHOT_DIR=<dir> saves screenshots there.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'home');

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

// The stand-in slicer: it only records. The test plays HomePanel's answers.
const SHIM = `(function(){ window.__sent = [];
    window.wx = { postMessage: function(m) { try { window.__sent.push(JSON.parse(m)); } catch (e) { window.__sent.push({ bad: m }); } } };
})();`;

const LIB = { type: 'library', hidden: 0, scanning: false, files_seen: 0, scanned_at: 1760000000,
    folders: [{ path: 'D:/Models', recursive: true, category: '', vendor: '', license: '', scanned: true, online: true, files: 3 },
              { path: '//diskstation/3d', recursive: true, category: '', vendor: '', license: '', scanned: true, online: false, files: 1 }],
    items: [1, 2, 3, 4].map(n => ({ id: 'f' + n, name: 'part' + n + '.stl', type: 'stl', root: 'D:/Models', rel_dir: '', mtime: 1760000000, size: 1000, added: 1760000000 })) };
const NO_LIB = { type: 'library', hidden: 0, scanning: false, files_seen: 0, scanned_at: 1760000000, folders: [], items: [] };

const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const files = { '/home/index.html': 'text/html; charset=utf-8', '/home/home.js': 'application/javascript', '/home/home.css': 'text/css' };
    if (files[u.pathname]) {
        res.writeHead(200, { 'Content-Type': files[u.pathname], 'Cache-Control': 'no-store' });
        return res.end(fs.readFileSync(path.join(WEB, path.basename(u.pathname))));
    }
    res.writeHead(404); res.end();
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
    const dbgPort = 9500 + Math.floor(Math.random() * 200);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'homevaults-'));
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
            const r = await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
            if (r.result && r.result.exceptionDetails) throw new Error('eval failed: ' + JSON.stringify(r.result.exceptionDetails).slice(0, 300));
            return r.result.result.value;
        };
        const key = async k => {
            const code = { Enter: 13, Escape: 27 }[k];
            await S('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code: k, windowsVirtualKeyCode: code, text: k === 'Enter' ? '\r' : undefined });
            await S('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k, windowsVirtualKeyCode: code });
        };
        const receive = msg => evalJs('window.HomeApp.receive(' + JSON.stringify(msg) + '); 1');
        const sent = async () => JSON.parse(await evalJs('JSON.stringify(window.__sent)'));
        const clearSent = () => evalJs('window.__sent = []; 1');
        const shot = async name => {
            if (!process.env.SHOT_DIR) return;
            const r = await S('Page.captureScreenshot', { format: 'png' });
            fs.writeFileSync(path.join(process.env.SHOT_DIR, name + '.png'), Buffer.from(r.result.data, 'base64'));
        };
        const exceptions = () => cdp.events.filter(e => e.method === 'Runtime.exceptionThrown');
        await S('Page.enable'); await S('Runtime.enable');
        await S('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
        await S('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });

        // ---- 1. load, rail -------------------------------------------------------------------
        await S('Page.navigate', { url: 'http://127.0.0.1:' + port + '/home/index.html' });
        await sleep(800);
        check('the page loads with no uncaught exception', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
        check('it says home_ready', (await sent()).some(m => m.command === 'home_ready'));
        const rail = JSON.parse(await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('.rail-item[data-section]'), b => b.dataset.section))`));
        check('the rail: Recent, Library, Vendors, Vaults, Print History', JSON.stringify(rail) === JSON.stringify(['recent', 'library', 'vendors', 'vaults', 'history']), rail.join());

        // ---- 2. a saved "vaults" opens Vaults ---------------------------------------------------
        await receive({ type: 'init', strings: {}, section: 'vaults', models: false });
        const open = JSON.parse(await evalJs(`JSON.stringify({ title: document.getElementById('title').textContent,
            shown: !document.getElementById('vaults').hidden, vendors: !document.getElementById('vendors').hidden,
            cur: document.querySelector('.rail-item[data-section=vaults]').getAttribute('aria-current') })`));
        check('a saved section "vaults" opens Vaults', open.title === 'Vaults' && open.shown && !open.vendors && open.cur === 'page', JSON.stringify(open));
        check('before the Library state arrives it says Loading', (await evalJs(`document.getElementById('vaults-empty-text').textContent`)) === 'Loading...');

        // ---- 3. empty state --------------------------------------------------------------------
        await receive(NO_LIB);
        const empty = JSON.parse(await evalJs(`JSON.stringify({ text: document.getElementById('vaults-empty-text').textContent,
            btn: !document.getElementById('vault-empty-add').hidden, cards: document.querySelectorAll('#vault-list .conn').length })`));
        check('empty: the explanation, an Add vault button, no cards', /^Vaults are your own model storage/.test(empty.text) && empty.btn && empty.cards === 0, JSON.stringify(empty));
        await shot('vaults_empty');

        // ---- 4. the Add vault chooser ----------------------------------------------------------
        const chooserOpen = () => evalJs(`!document.getElementById('vault-add').hidden`);
        await evalJs(`document.getElementById('vault-empty-add').click(); 1`);
        check('the empty state\'s Add vault opens the chooser', await chooserOpen());
        check('the chooser has the focus on its first choice', (await evalJs('document.activeElement.id')) === 'vault-add-folder');
        await key('Escape');
        check('Escape closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        check('the header\'s Add vault opens it too', await chooserOpen());
        await evalJs(`document.getElementById('vault-add').click(); 1`);
        check('a click on the backdrop closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); document.getElementById('vault-add-cancel').click(); 1`);
        check('Cancel closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        await clearSent();
        await key('Enter');
        await sleep(50);
        const fs1 = await sent();
        check('Folder or network share (from the keyboard) sends library_add_folder and closes',
              fs1.length === 1 && fs1[0].command === 'library_add_folder' && !(await chooserOpen()), JSON.stringify(fs1));
        const connectorChoices = await evalJs(`document.querySelectorAll('#vault-add [id^=vault-import], #vault-add-api').length`);
        check('the chooser offers no vendor connector (those stay in Vendors)', connectorChoices === 0, connectorChoices);

        // ---- 5. the folders card -----------------------------------------------------------------
        await receive(LIB);
        const cards = JSON.parse(await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#vault-list .conn'), c => ({
            title: c.querySelector('.title').textContent, kind: (c.querySelector('.kind') || {}).textContent,
            lines: [].map.call(c.querySelectorAll('.line'), l => l.textContent), warn: [].map.call(c.querySelectorAll('.line.warn'), l => l.textContent),
            role: c.getAttribute('role') })))`));
        await shot('vaults_folders');
        check('one card: Folders and network shares, a list item of a list', cards.length === 1 && cards[0].title === 'Folders and network shares' &&
              cards[0].role === 'listitem' && (await evalJs(`document.getElementById('vault-list').getAttribute('role')`)) === 'list', JSON.stringify(cards));
        check('the folders card counts folders and files', cards[0].lines[0] === '2 folders \u00b7 4 files' && cards[0].kind === 'Library', cards[0].lines.join('|'));
        check('the folders card names the unreachable share', cards[0].warn.length === 1 && /^3d: Not reachable/.test(cards[0].warn[0]), cards[0].warn.join('|'));
        check('no empty state once there is a card', await evalJs(`document.getElementById('vaults-empty').hidden`));
        await clearSent();
        await evalJs(`document.querySelector('#vault-list .conn.folders .btn').click(); 1`);
        const lib = JSON.parse(await evalJs(`JSON.stringify({ title: document.getElementById('title').textContent, sent: window.__sent })`));
        check('Show in Library opens the Library and remembers it', lib.title === 'Library' && lib.sent.some(m => m.command === 'home_section' && m.section === 'library'), JSON.stringify(lib));
        await evalJs(`document.querySelector('.rail-item[data-section=vaults]').click(); 1`);
        check('the rail saves the section as "vaults"', (await sent()).some(m => m.command === 'home_section' && m.section === 'vaults'));
        await evalJs(`document.querySelectorAll('#vault-list .conn.folders .btn')[1].click(); 1`);
        check('Manage folders opens the Library folder manager', !(await evalJs(`document.getElementById('folders').hidden`)));
        await evalJs(`document.getElementById('folders-done').click(); 1`);

        // ---- 6. Vendors is untouched -------------------------------------------------------------
        await clearSent();
        await receive({ type: 'show_section', section: 'vendors' });
        const ven = JSON.parse(await evalJs(`JSON.stringify({ title: document.getElementById('title').textContent, shown: !document.getElementById('vendors').hidden,
            imp: !!document.getElementById('ven-import'), add: (document.getElementById('ven-add') || {}).textContent })`));
        check('show_section "vendors" still opens Vendors, with Import and Add connector', ven.title === 'Vendors' && ven.shown && ven.imp && ven.add === 'Add connector', JSON.stringify(ven));
        check('and saves "vendors"', (await sent()).some(m => m.command === 'home_section' && m.section === 'vendors'));
        await evalJs(`document.querySelector('.rail-item[data-section=vaults]').click(); 1`);

        // ---- 7. contrast ------------------------------------------------------------------------
        // The text this section adds. The accent colours (primary buttons, the active rail item) are the
        // whole page's and are not judged here.
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        const pairs = `(function(){
            function bgOf(n) { while (n) { var b = getComputedStyle(n).backgroundColor; if (b && b !== 'rgba(0, 0, 0, 0)' && b !== 'transparent') return b; n = n.parentElement; } return getComputedStyle(document.body).backgroundColor; }
            var out = [];
            ['#vault-list .conn .title', '#vault-list .conn .kind', '#vault-list .conn .line', '#vault-list .conn .line.warn', '#vault-list .conn .btn:not(.primary)',
             '.choice b', '.choice span', '.dialog.chooser h2', '.dialog.chooser .hint']
              .forEach(function(sel) { document.querySelectorAll(sel).forEach(function(n) { if (!n.offsetParent) return;
                  out.push({ sel: sel, fg: getComputedStyle(n).color, bg: bgOf(n) }); }); });
            return JSON.stringify(out); })()`;
        for (const theme of ['light', 'dark']) {
            await evalJs(`document.documentElement.classList.toggle('dark', ${theme === 'dark'}); 1`);
            await shot('vaults_chooser_' + theme);
            const list = JSON.parse(await evalJs(pairs));
            const worst = {};
            list.forEach(p => {
                const f = parseColor(p.fg), b = parseColor(p.bg);
                if (!f || !b) return;
                const r = contrast(f, b);
                if (!(p.sel in worst) || r < worst[p.sel].r) worst[p.sel] = { r, fg: p.fg, bg: p.bg };
            });
            const low = Object.keys(worst).filter(k => worst[k].r < 4.5);
            console.log('# ' + theme + ' contrast: ' + Object.keys(worst).map(k => k + ' ' + worst[k].r.toFixed(2)).join(', '));
            check(theme + ': every new text / background pair is >= 4.5:1', low.length === 0 && Object.keys(worst).length >= 8,
                  low.map(k => k + ' ' + worst[k].r.toFixed(2) + ' ' + worst[k].fg + ' on ' + worst[k].bg).join('; ') || Object.keys(worst).length);
        }
        check('no uncaught exception over the run', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
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

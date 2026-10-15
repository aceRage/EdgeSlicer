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

// What HomeVaults::send_state() sends: specs without secrets, opaque file keys, the shipped built-in.
const SYNOLOGY = Object.assign(JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'resources', 'vaults', 'synology.json'), 'utf8')), { id: 'synology' });
SYNOLOGY.spec.builtin = 'synology';
const NO_VAULTS = { type: 'vaults', vaults: [], files: [], builtins: [], downloads_default: 'C:/Users/me/Downloads/EdgeSlicer Vaults' };
const NOW_S = Math.floor(Date.now() / 1000);
const VAULTS = { type: 'vaults', builtins: [SYNOLOGY], downloads_default: 'C:/Users/me/Downloads/EdgeSlicer Vaults',
    vaults: [
        { id: 'nas-1', name: 'DiskStation', kind: 'webdav', builtin: 'synology', address: 'http://192.168.1.20:5005/models/', bucket: '', prefix: '',
          spec: { format: 'edgeslicer-vault', version: 1, id: 'nas-1', name: 'DiskStation', kind: 'webdav', url: 'http://192.168.1.20:5005/models/', auth: 'basic',
                  recursive: true, max_depth: 6, builtin: 'synology', downloads_dir: '' },
          slots: [{ key: 'user', label: 'User name', set: true }, { key: 'password', label: 'Password', set: false }], secure: true, syncing: false,
          synced_at: NOW_S - 3600, count: 2, partial: false, last_error: '', http: true, http_ok: false, sas_expires: 0,
          downloads: 'C:/Users/me/Downloads/EdgeSlicer Vaults' },
        { id: 'az-2', name: 'Azure prints', kind: 'azure_blob', builtin: '', address: 'https://acct.blob.core.windows.net/models', bucket: '', prefix: 'prints/',
          spec: { format: 'edgeslicer-vault', version: 1, id: 'az-2', name: 'Azure prints', kind: 'azure_blob', url: 'https://acct.blob.core.windows.net/models',
                  prefix: 'prints/', auth: 'sas', recursive: true, max_depth: 6, builtin: '', downloads_dir: '' },
          slots: [{ key: 'sas', label: 'SAS link', set: true }], secure: true, syncing: false, synced_at: NOW_S - 60, count: 1, partial: false, last_error: '',
          http: false, http_ok: false, sas_expires: NOW_S + 3 * 86400, downloads: 'C:/Users/me/Downloads/EdgeSlicer Vaults' },
        { id: 's3-3', name: 'S3 prints', kind: 's3', builtin: '', address: 'https://s3.eu-west-1.amazonaws.com', bucket: 'prints', prefix: '',
          spec: { format: 'edgeslicer-vault', version: 1, id: 's3-3', name: 'S3 prints', kind: 's3', url: 'https://s3.eu-west-1.amazonaws.com', bucket: 'prints',
                  region: 'eu-west-1', addressing: 'path', prefix: '', auth: 'sigv4', recursive: true, max_depth: 6, builtin: '', downloads_dir: 'D:/Vaults/S3 prints' },
          slots: [{ key: 'access_key_id', label: 'Access key ID', set: true }, { key: 'secret_access_key', label: 'Secret access key', set: true }],
          secure: true, syncing: false, synced_at: 0, count: 0, partial: false, last_error: 'Access denied (HTTP 403, AccessDenied). Check the credentials and what they are allowed to read.',
          http: false, http_ok: false, sas_expires: 0, downloads: 'D:/Vaults/S3 prints' },
    ],
    files: [
        { key: 'f-nas', vault: 'nas-1', name: 'gear v2.stl', folder: 'parts', size: 120000, mtime: NOW_S - 7200, type: 'stl' },
        { key: 'f-nas2', vault: 'nas-1', name: '<img src=x onerror="window.__xss=1">.3mf', folder: '', size: 5000, mtime: NOW_S - 86400, type: '3mf' },
        { key: 'f-az', vault: 'az-2', name: 'bracket.3mf', folder: 'tools', size: 900000, mtime: NOW_S - 600, type: '3mf' },
    ] };

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
    let cdp = null;
    try {
        let ver;
        for (let i = 0; i < 300 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        if (!ver) throw new Error('the headless browser did not start within 60 s');
        cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
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
        await receive(NO_VAULTS);
        const empty = JSON.parse(await evalJs(`JSON.stringify({ text: document.getElementById('vaults-empty-text').textContent,
            btn: !document.getElementById('vault-empty-add').hidden, cards: document.querySelectorAll('#vault-list .conn').length })`));
        check('empty: the explanation, an Add vault button, no cards', /^Vaults are your own model storage/.test(empty.text) && empty.btn && empty.cards === 0, JSON.stringify(empty));
        await shot('vaults_empty');

        // ---- 4. the Add vault chooser ----------------------------------------------------------
        const chooserOpen = () => evalJs(`!document.getElementById('vault-add').hidden`);
        await evalJs(`document.getElementById('vault-empty-add').click(); 1`);
        check('the empty state\'s Add vault opens the chooser', await chooserOpen());
        check('the chooser has the focus on its first choice', (await evalJs('document.activeElement.id')) === 'vault-add-webdav');
        await key('Escape');
        check('Escape closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        check('the header\'s Add vault opens it too', await chooserOpen());
        await evalJs(`document.getElementById('vault-add').click(); 1`);
        check('a click on the backdrop closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); document.getElementById('vault-add-cancel').click(); 1`);
        check('Cancel closes it', !(await chooserOpen()));
        await evalJs(`document.getElementById('vault-add-btn').click(); document.getElementById('vault-add-folder').focus(); 1`);
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

        // ---- 6. storage vaults: cards, files, editor -----------------------------------------------
        await receive(VAULTS);
        await sleep(50);
        const vc = JSON.parse(await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#vault-list .conn.vault'), c => ({
            id: c.dataset.vault, title: c.querySelector('.title').textContent, kind: c.querySelector('.kind').textContent,
            lines: [].map.call(c.querySelectorAll('.line'), l => l.textContent), warn: [].map.call(c.querySelectorAll('.line.warn'), l => l.textContent),
            err: [].map.call(c.querySelectorAll('.line.err'), l => l.textContent), btns: [].map.call(c.querySelectorAll('.row .btn'), b => b.textContent),
            imgs: c.querySelectorAll('img').length })))`));
        await shot('vaults_cards');
        check('three vault cards, then the folders card', vc.length === 3 && (await evalJs(`document.querySelectorAll('#vault-list .conn').length`)) === 4, vc.length);
        check('kind labels: Synology NAS (built-in), Azure Blob, S3-compatible', vc[0].kind === 'Synology NAS' && vc[1].kind === 'Azure Blob' && vc[2].kind === 'S3-compatible',
              vc.map(v => v.kind).join('|'));
        check('a vault card shows its address, never a token', vc[0].lines[0] === 'http://192.168.1.20:5005/models/' && !JSON.stringify(vc).includes('sig='), vc[0].lines[0]);
        check('the http vault says it is unencrypted', vc[0].warn.some(w => /Unencrypted \(http\)/.test(w)), vc[0].warn.join('|'));
        check('missing credentials are named', vc[0].warn.some(w => w === 'Credentials: Password not set'), vc[0].warn.join('|'));
        check('a set credential says where it is kept', vc[1].lines.includes("Credentials: saved in your system's credential store"), vc[1].lines.join('|'));
        check('a SAS link close to expiry is a warning', vc[1].warn.some(w => /^SAS link expires/.test(w)), vc[1].warn.join('|'));
        check('the last error is shown', vc[2].err.length === 1 && /HTTP 403/.test(vc[2].err[0]), vc[2].err.join('|'));
        check('the downloads folder is named', vc[2].lines.some(l => l === 'Downloads to S3 prints'), vc[2].lines.join('|'));
        check('vault text is text, never markup', vc.every(v => v.imgs === 0) && !(await evalJs('!!window.__xss')));
        check('Fetch files and a Set button per missing credential', JSON.stringify(vc[0].btns) === JSON.stringify(['Fetch files', 'Change User name', 'Set Password']), vc[0].btns.join('|'));
        await clearSent();
        await evalJs(`document.querySelector('.conn.vault[data-vault=nas-1] .row .btn').click(); document.querySelectorAll('.conn.vault[data-vault=nas-1] .row .btn')[2].click(); 1`);
        let s6 = await sent();
        check('Fetch files sends vault_sync, Set Password sends vault_secret (the page never sees the secret)',
              s6.length === 2 && s6[0].command === 'vault_sync' && s6[0].id === 'nas-1' && s6[1].command === 'vault_secret' && s6[1].slot === 'password', JSON.stringify(s6));
        const menuOf = async (id) => {
            await evalJs(`document.querySelector('.conn.vault[data-vault=${id}] .more').click(); 1`);
            return JSON.parse(await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#menu button'), b => b.textContent))`));
        };
        const m6 = await menuOf('az-2');
        check('the vault menu: Test, Edit, Open downloads folder, Forget, Remove', JSON.stringify(m6) === JSON.stringify(['Test connection', 'Edit', 'Open downloads folder', 'Forget credentials', 'Remove vault']), m6.join('|'));
        await clearSent();
        await evalJs(`[].find.call(document.querySelectorAll('#menu button'), b => b.textContent === 'Test connection').click(); 1`);
        await menuOf('az-2');
        await evalJs(`[].find.call(document.querySelectorAll('#menu button'), b => b.textContent === 'Remove vault').click(); 1`);
        s6 = await sent();
        check('Test sends vault_test, Remove sends vault_delete (the slicer asks first)', s6.map(m => m.command).join() === 'vault_test,vault_delete' && s6[1].id === 'az-2', JSON.stringify(s6));

        // Files.
        const files = () => evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#vault-grid .card'), c => ({ key: c.dataset.id, name: c.querySelector('.name').textContent,
            sub: c.querySelector('.sub') ? c.querySelector('.sub').textContent : '', badge: c.querySelector('.badge').textContent, imgs: c.querySelectorAll('img').length })))`);
        let fl = JSON.parse(await files());
        check('the files of every vault, newest first', fl.length === 3 && fl[0].key === 'f-az' && fl[2].key === 'f-nas2', fl.map(f => f.key).join());
        check('a file card: name, vault and folder, type', fl[1].name === 'gear v2' && fl[1].sub === 'DiskStation \u00b7 parts' && fl[1].badge === 'STL', JSON.stringify(fl[1]));
        check('a file name is text, never markup', fl.every(f => f.imgs === 0) && fl[2].name.includes('<img'), fl[2].name);
        check('the status counts the files', (await evalJs(`document.getElementById('vault-status').textContent`)) === '3 files');
        await clearSent();
        await evalJs(`document.querySelector('#vault-grid .card[data-id=f-nas]').click(); 1`);
        s6 = await sent();
        check('a click on a file is Download & Open', s6.length === 1 && s6[0].command === 'vault_download' && s6[0].key === 'f-nas' && s6[0].open === true, JSON.stringify(s6));
        await clearSent();
        await evalJs(`document.querySelector('#vault-grid .card[data-id=f-nas] .more').click(); 1`);
        await evalJs(`[].find.call(document.querySelectorAll('#menu button'), b => b.textContent === 'Download').click(); 1`);
        s6 = await sent();
        check('Download from the menu saves without opening', s6.length === 1 && s6[0].command === 'vault_download' && !s6[0].open, JSON.stringify(s6));
        const chipCount = await evalJs(`document.querySelectorAll('#vault-filters .chip').length`);
        check('filters by vault and by type', chipCount === 2 + 2, chipCount);
        await evalJs(`[].find.call(document.querySelectorAll('#vault-filters .chip'), c => c.firstChild.textContent === 'Azure prints').click(); 1`);
        fl = JSON.parse(await files());
        check('the vault filter keeps that vault\'s files', fl.length === 1 && fl[0].key === 'f-az', fl.map(f => f.key).join());
        await evalJs(`document.querySelector('#vault-filters .chip.clear').click(); 1`);
        await evalJs(`(function(){ var s = document.getElementById('search'); s.value = 'gear'; s.dispatchEvent(new Event('input')); return 1; })()`);
        await sleep(250);
        fl = JSON.parse(await files());
        check('search finds a file by name', fl.length === 1 && fl[0].key === 'f-nas', fl.map(f => f.key).join());
        await evalJs(`(function(){ var s = document.getElementById('search'); s.value = ''; s.dispatchEvent(new Event('input')); return 1; })()`);
        await sleep(250);

        // The chooser with the built-in, and the editor.
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        const ch = JSON.parse(await evalJs(`JSON.stringify({ first: document.activeElement.id, ids: [].map.call(document.querySelectorAll('#vault-add .choices button.choice'), b => b.id),
            tag: (document.querySelector('#vault-builtin-synology .tag') || {}).textContent })`));
        check('the chooser: Synology (built-in) first, then WebDAV, Azure Blob, S3, Folder',
              JSON.stringify(ch.ids) === JSON.stringify(['vault-builtin-synology', 'vault-add-webdav', 'vault-add-azure', 'vault-add-s3', 'vault-add-folder']) &&
              ch.first === 'vault-builtin-synology' && ch.tag === 'Built-in', JSON.stringify(ch));
        await evalJs(`document.getElementById('vault-builtin-synology').click(); 1`);
        const ed = JSON.parse(await evalJs(`JSON.stringify({ open: !document.getElementById('vault-editor').hidden, chooser: !document.getElementById('vault-add').hidden,
            hints: document.querySelectorAll('#vault-editor-body ul.hints li').length, name: document.getElementById('vault-field-name').value,
            ph: document.getElementById('vault-field-url').placeholder, bucket: !!document.getElementById('vault-field-bucket'),
            prefix: !!document.getElementById('vault-field-prefix'), auth: [].map.call(document.querySelectorAll('#vault-field-auth option'), o => o.value),
            pw: document.querySelectorAll('#vault-editor-body input[type=password]').length, focus: document.activeElement.id })`));
        await shot('vaults_editor_synology');
        check('Synology opens the editor with its hints, name and address example', ed.open && !ed.chooser && ed.hints === SYNOLOGY.hints.length && ed.hints > 0 && ed.name === 'Synology NAS' &&
              ed.ph === 'https://diskstation.local:5006/models/' && ed.focus === 'vault-field-name', JSON.stringify(ed));
        check('a WebDAV editor has no bucket or prefix; sign-in basic / bearer / none; no password field in the page',
              !ed.bucket && !ed.prefix && JSON.stringify(ed.auth) === JSON.stringify(['basic', 'bearer', 'none']) && ed.pw === 0, JSON.stringify(ed));
        await evalJs(`(function(){ var u = document.getElementById('vault-field-url'); u.value = 'http://192.168.1.20:5005/models'; u.dispatchEvent(new Event('input'));
            var n = document.getElementById('vault-field-name'); n.value = 'Home NAS'; n.dispatchEvent(new Event('input')); return 1; })()`);
        await clearSent();
        await evalJs(`document.getElementById('vault-field-downloads_dir').parentNode.parentNode.parentNode.querySelector('.choice-row .btn').click(); 1`);
        s6 = await sent();
        check('Browse asks the slicer for a folder', s6.length === 1 && s6[0].command === 'vault_pick_dir', JSON.stringify(s6));
        await receive({ type: 'vault_dir', path: 'D:/Prints/From NAS' });
        check('the picked folder fills the field', (await evalJs(`document.getElementById('vault-field-downloads_dir').value`)) === 'D:/Prints/From NAS');
        await clearSent();
        await evalJs(`document.getElementById('vault-editor-save').click(); 1`);
        s6 = await sent();
        const saved = s6.length === 1 ? s6[0].spec : {};
        check('Save sends the spec: kind, built-in, address, sign-in, folder, and no secret',
              s6.length === 1 && s6[0].command === 'vault_save' && saved.kind === 'webdav' && saved.builtin === 'synology' && saved.url === 'http://192.168.1.20:5005/models' &&
              saved.auth === 'basic' && saved.name === 'Home NAS' && saved.downloads_dir === 'D:/Prints/From NAS' && !('id' in saved) &&
              !/password|secret|token|sig=/i.test(JSON.stringify(saved)), JSON.stringify(s6));
        await receive({ type: 'vault_invalid', error: 'Plain http is only allowed for a device on your local network.' });
        check('a refused save shows why and keeps the editor open', !(await evalJs(`document.getElementById('vault-editor-error').hidden`)) &&
              !(await evalJs(`document.getElementById('vault-editor').hidden`)));
        await receive({ type: 'vault_saved', id: 'home-nas-1' });
        check('a saved vault closes the editor', await evalJs(`document.getElementById('vault-editor').hidden`));

        const editorFor = async (btn) => {
            await evalJs(`document.getElementById('vault-add-btn').click(); document.getElementById('${btn}').click(); 1`);
            return JSON.parse(await evalJs(`JSON.stringify({ title: document.getElementById('vault-editor-title').textContent,
                bucket: !!document.getElementById('vault-field-bucket'), region: !!document.getElementById('vault-field-region'),
                addressing: [].map.call(document.querySelectorAll('#vault-field-addressing option'), o => o.value), prefix: !!document.getElementById('vault-field-prefix'),
                auth: [].map.call(document.querySelectorAll('#vault-field-auth option'), o => o.value), ph: document.getElementById('vault-field-url').placeholder,
                hint: document.getElementById('vault-editor-body').textContent })`));
        };
        const az = await editorFor('vault-add-azure');
        check('Azure Blob: a prefix, sign-in SAS link or none, the container example', az.prefix && !az.bucket && JSON.stringify(az.auth) === JSON.stringify(['sas', 'none']) &&
              az.ph === 'https://account.blob.core.windows.net/models' && az.title === 'Azure Blob container', JSON.stringify(az));
        await key('Escape');
        check('Escape closes the editor', await evalJs(`document.getElementById('vault-editor').hidden`));
        const s3 = await editorFor('vault-add-s3');
        await shot('vaults_editor_s3');
        check('S3: bucket, region, addressing, prefix, access key or none, a read-only key hint', s3.bucket && s3.region && s3.prefix &&
              JSON.stringify(s3.addressing) === JSON.stringify(['path', 'virtual']) && JSON.stringify(s3.auth) === JSON.stringify(['sigv4', 'none']) &&
              /only read this bucket/.test(s3.hint), JSON.stringify(s3));
        await evalJs(`document.getElementById('vault-editor-cancel').click(); 1`);
        await menuOf('nas-1');
        await evalJs(`[].find.call(document.querySelectorAll('#menu button'), b => b.textContent === 'Edit').click(); 1`);
        const edit = JSON.parse(await evalJs(`JSON.stringify({ name: document.getElementById('vault-field-name').value, url: document.getElementById('vault-field-url').value,
            slots: [].map.call(document.querySelectorAll('#vault-editor-slots .slot'), s => s.textContent) })`));
        check('Edit fills the editor from the vault and lists its credentials', edit.name === 'DiskStation' && edit.url === 'http://192.168.1.20:5005/models/' &&
              edit.slots.length === 2 && /Password: not set/.test(edit.slots[1]), JSON.stringify(edit));
        await clearSent();
        await evalJs(`document.getElementById('vault-editor-save').click(); 1`);
        s6 = await sent();
        check('saving an edited vault keeps its id', s6.length === 1 && s6[0].spec.id === 'nas-1', JSON.stringify(s6));
        await receive({ type: 'vault_saved', id: 'nas-1' });
        await receive({ type: 'vault_notice', text: 'DiskStation: Connected.', error: false });
        check('a notice is shown', (await evalJs(`document.getElementById('toast').textContent`)) === 'DiskStation: Connected.');

        // ---- 7. Vendors is untouched -------------------------------------------------------------
        await clearSent();
        await receive({ type: 'show_section', section: 'vendors' });
        const ven = JSON.parse(await evalJs(`JSON.stringify({ title: document.getElementById('title').textContent, shown: !document.getElementById('vendors').hidden,
            imp: !!document.getElementById('ven-import'), add: (document.getElementById('ven-add') || {}).textContent })`));
        check('show_section "vendors" still opens Vendors, with Import and Add connector', ven.title === 'Vendors' && ven.shown && ven.imp && ven.add === 'Add connector', JSON.stringify(ven));
        check('and saves "vendors"', (await sent()).some(m => m.command === 'home_section' && m.section === 'vendors'));
        await evalJs(`document.querySelector('.rail-item[data-section=vaults]').click(); 1`);

        // ---- 8. contrast ------------------------------------------------------------------------
        // The text this section adds, with its primary buttons, active chips and the active rail item on
        // the accent colours made for text (#478, folded in).
        await evalJs(`[].find.call(document.querySelectorAll('#vault-filters .chip'), c => c.firstChild.textContent === 'DiskStation').click(); 1`);
        check('a picked filter chip is active', (await evalJs(`document.querySelectorAll('#vault-filters .chip.active').length`)) === 1);
        await evalJs(`document.getElementById('vault-add-btn').click(); 1`);
        const pairs = `(function(){
            function bgOf(n) { while (n) { var b = getComputedStyle(n).backgroundColor; if (b && b !== 'rgba(0, 0, 0, 0)' && b !== 'transparent') return b; n = n.parentElement; } return getComputedStyle(document.body).backgroundColor; }
            var out = [];
            ['#vault-list .conn .title', '#vault-list .conn .kind', '#vault-list .conn .line', '#vault-list .conn .line.warn', '#vault-list .conn .line.err',
             '#vault-list .conn .btn', '#vault-add-btn', '.rail-item.active', '#vault-grid .card .name', '#vault-grid .card .sub', '#vault-grid .card .meta',
             '#vault-filters .chip', '#vault-grid .badge', '.choice b', '.choice span', '.choice .tag', '.dialog.chooser h2', '.dialog.chooser .hint']
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
        // msedge.exe may hand the window to a broker and exit at once: close the browser itself over
        // the protocol, so no headless browser is left behind.
        if (cdp) { try { await Promise.race([cdp.send('Browser.close'), sleep(2000)]); } catch (e) {} }
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

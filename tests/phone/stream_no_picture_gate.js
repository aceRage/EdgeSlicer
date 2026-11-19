// Gate: a stream tile that gets no picture says why, instead of staying black
// (resources/web/orca/stream_center.html + player.html), on the phone view of the page.
//
// A mock hub (plain Node http, no dependencies) serves the real shipped page at /r/<token>/, the
// real player at /stream.html, a go2rtc-like WebSocket at /api/ws and the Bambu relay's MJPEG at
// /r/<token>/bambu. Streams are named by what they do:
//   good     answers {"type":"mjpeg"} with a binary frame (a picture arrives)
//   refused  answers with {"type":"error","value":"... connection refused"} and nothing else
//   auth     the same with "401 Unauthorized"
//   secret   an error whose text carries a password (must never reach the page)
//   silent   accepts the socket and says nothing
// Bambu relay: id=bbl_ok streams frames at once; id=bbl_hang sends the 200 head and nothing else
// (a printer that accepted the connection but sends no video).
// The page runs with ?nopicms=2000 so the gate does not wait 10 s. A headless Edge / Chrome is
// driven over the DevTools protocol (Node 22's built-in WebSocket). No printer, no go2rtc, no
// real camera, no EdgeSlicer GUI.
//
// Run: node tests/phone/stream_no_picture_gate.js   (skips, exit 0, when no Chromium is found)
//  as: sh /c/Dev/edgebuild/edgebuild gate-run -- node tests/phone/stream_no_picture_gate.js
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const crypto = require('crypto');
const { spawn } = require('child_process');

const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'orca');
const TOKEN = 'abc123';
const BASE = '/r/' + TOKEN + '/';
const NOPICMS = 2000;

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

// ---- the mock hub ------------------------------------------------------------------------------
const HOSTS = [
    { id: 'x1',    alias: 'X1 Carbon', rkind: 'go2rtc', rcam: 'rtsps', rname: 'refused', rurl: '' },
    { id: 'cam',   alias: 'Garage',    rkind: 'go2rtc', rcam: 'ipcam', rname: 'auth',    rurl: '' },
    { id: 'old',   alias: 'Old hub',   rkind: 'go2rtc',                rname: 'silent',  rurl: '' },
    { id: 'a1',    alias: 'A1 hang',   rkind: 'p1', rcam: 'p1', rname: '', rurl: '' },
    { id: 'a1ok',  alias: 'A1 fine',   rkind: 'p1', rcam: 'p1', rname: '', rurl: '' },
];
const bambuIds = [];   // every id the relay was asked for, with its query string
let hangOpen = 0;
const GIF = Buffer.from('R0lGODlhAQABAIAAAAUEBAAAACwAAAAAAQABAAACAkQBADs=', 'base64');

const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (type, body, code) => { res.writeHead(code || 200, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
    if (u.pathname === BASE || u.pathname === BASE + 'index.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'stream_center.html')));
    if (u.pathname === '/stream.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'player.html')));
    if (u.pathname === '/video-stream.js' || u.pathname === '/video-rtc.js')
        return send('application/javascript', fs.readFileSync(path.join(WEB, u.pathname)));
    if (u.pathname === BASE + 'state')
        return send('application/json', JSON.stringify({ hosts: HOSTS, active: HOSTS.map(h => h.id), webrtc: false, quality: [], quality_mjpeg: true, printers: [] }));
    if (u.pathname === BASE + 'bambu') {
        const id = u.searchParams.get('id');
        bambuIds.push(u.search);
        res.writeHead(200, { 'Content-Type': 'multipart/x-mixed-replace; boundary=frame', 'Cache-Control': 'no-store' });
        if (id === 'a1ok') {
            const t = setInterval(() => {
                res.write('--frame\r\nContent-Type: image/gif\r\nContent-Length: ' + GIF.length + '\r\n\r\n');
                res.write(GIF); res.write('\r\n');
            }, 100);
            res.on('close', () => clearInterval(t));
        } else {
            hangOpen++;
            res.flushHeaders();
            res.on('close', () => { hangOpen--; });
        }
        return;
    }
    send('application/json', '{}', 404);
});

function wsFrame(opcode, payload) {
    const head = payload.length < 126 ? Buffer.from([0x80 | opcode, payload.length])
        : Buffer.from([0x80 | opcode, 126, payload.length >> 8, payload.length & 255]);
    return Buffer.concat([head, payload]);
}
server.on('upgrade', (req, socket) => {
    const u = new URL(req.url, 'http://x');
    if (u.pathname !== '/api/ws') { socket.destroy(); return; }
    const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
    const src = u.searchParams.get('src') || '';
    const errText = {
        refused: 'streams: dial tcp 192.168.1.50:322: connect: connection refused',
        auth: 'streams: wrong user/pass: 401 Unauthorized',
        secret: 'streams: rtspx://bblp:HUNTER2PASSWORD@192.168.1.50:322/streaming/live/1: something odd happened',
    }[src];
    let pending = Buffer.alloc(0), told = false;
    const sendErr = () => { if (errText && !told) { told = true; socket.write(wsFrame(1, Buffer.from(JSON.stringify({ type: 'error', value: errText })))); } };
    setTimeout(sendErr, 300);
    socket.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        for (;;) {
            if (pending.length < 2) return;
            let len = pending[1] & 0x7f, off = 2;
            if (len === 126) { if (pending.length < 4) return; len = pending.readUInt16BE(2); off = 4; }
            else if (len === 127) { if (pending.length < 10) return; len = Number(pending.readBigUInt64BE(2)); off = 10; }
            const masked = !!(pending[1] & 0x80);
            if (masked) off += 4;
            if (pending.length < off + len) return;
            const opcode = pending[0] & 0x0f;
            let body = pending.subarray(off, off + len);
            if (masked) { const m = pending.subarray(off - 4, off); body = Buffer.from(body.map((b, i) => b ^ m[i % 4])); }
            pending = pending.subarray(off + len);
            if (opcode === 8) { socket.end(Buffer.from([0x88, 0x00])); return; }
            if (opcode === 1) {
                let msg = {}; try { msg = JSON.parse(body.toString()); } catch (e) {}
                if (src === 'good' && msg.type === 'mjpeg') socket.write(wsFrame(2, GIF));
                if (errText) sendErr();
            }
        }
    });
    socket.on('error', () => {});
});

// ---- a tiny DevTools client --------------------------------------------------------------------
class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data); if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); } }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'nopic-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--autoplay-policy=no-user-gesture-required',
        '--disable-background-timer-throttling', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const open = async () => {
            const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
            const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
            const S = (m, p) => cdp.send(m, p, sessionId);
            const ev = async expr => (await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result.result.value;
            await S('Page.enable'); await S('Runtime.enable');
            return { S, ev, sessionId };
        };

        // ---- 1. the player on its own: what it tells its embedder ------------------------------
        // Opened as a top-level page, parent === window, so its postMessage lands on itself and a
        // recorder installed before it loads sees exactly what a real embedder would.
        const player = async (src, mode) => {
            const t = await open();
            await t.S('Page.addScriptToEvaluateOnNewDocument', { source: 'window.__msgs = []; window.addEventListener("message", function(e) { __msgs.push(e.data); });' });
            await t.S('Page.navigate', { url: 'http://127.0.0.1:' + port + '/stream.html?src=' + src + '&mode=' + mode + '&nopicms=' + NOPICMS });
            return t;
        };
        const msgs = async t => JSON.parse(await t.ev('JSON.stringify(window.__msgs || [])'));
        {
            const good = await player('good', 'mjpeg');
            await sleep(NOPICMS + 1500);
            const g = await msgs(good);
            check('player: a picture arriving is reported', g.some(m => m.snorcaPicture === true), JSON.stringify(g));
            check('player: and a tile with a picture never reports "no picture"', !g.some(m => m.snorcaNoPicture), JSON.stringify(g));
        }
        const reasonOf = async (src) => {
            const t = await player(src, 'mse');
            await sleep(NOPICMS + 1500);
            return msgs(t);
        };
        {
            const m = (await reasonOf('refused')).filter(x => x.snorcaNoPicture);
            check('player: connection refused is reported with its real reason', m.length === 1 && m[0].reason === 'refused', JSON.stringify(m));
            const a = (await reasonOf('auth')).filter(x => x.snorcaNoPicture);
            check('player: 401 is reported as auth', a.length === 1 && a[0].reason === 'auth', JSON.stringify(a));
            const s = (await reasonOf('silent')).filter(x => x.snorcaNoPicture);
            check('player: a silent camera is reported with no invented reason', s.length === 1 && s[0].reason === '', JSON.stringify(s));
            const raw = await reasonOf('secret');
            const sec = raw.filter(x => x.snorcaNoPicture);
            check('player: an error it cannot classify gives no reason', sec.length === 1 && sec[0].reason === '', JSON.stringify(sec));
            check('player: go2rtc\'s own text (with a password in it) never leaves the player', JSON.stringify(raw).indexOf('HUNTER2PASSWORD') === -1);
        }

        // ---- 2. the phone view of the page --------------------------------------------------------
        const pg = await open();
        await pg.S('Emulation.setDeviceMetricsOverride', { width: 390, height: 1600, deviceScaleFactor: 1, mobile: true });
        await pg.S('Page.navigate', { url: 'http://127.0.0.1:' + port + BASE + '?nopicms=' + NOPICMS });
        const cells = () => pg.ev(`JSON.stringify(Array.prototype.map.call(document.querySelectorAll('#grid .cell[data-id]'), function(c) {
            var o = c.querySelector('.nopic'); return { id: c.dataset.id, shown: !!o && o.style.display === 'flex', text: o ? o.firstChild.textContent : null,
              retry: !!(o && o.querySelector('button')) }; }))`).then(JSON.parse);
        const byId = list => Object.fromEntries(list.map(c => [c.id, c]));

        // No flash: through the first 1.2 s of a normal start nothing may be shown.
        await sleep(1200);
        let early = byId(await cells());
        check('the five tiles were built', Object.keys(early).length === 5, Object.keys(early).join());
        check('no hint shows during a normal start', Object.values(early).every(c => !c.shown), JSON.stringify(early));

        await sleep(NOPICMS + 1500);
        const late = byId(await cells());
        console.log('# tiles after the window: ' + JSON.stringify(late));
        check('Bambu X1/H2 tile: LAN Mode Liveview text, with the hub\'s real reason',
            late.x1.shown && late.x1.text.indexOf('Connection refused. Check that LAN Mode Liveview is on at the printer') === 0 && /access code is current/.test(late.x1.text), late.x1.text);
        check('generic tile: names the camera and the real reason', late.cam.shown && late.cam.text === 'No video from Garage. Login refused.', late.cam.text);
        check('a tile with no camera type (older hub) and no reason says the generic line', late.old.shown && late.old.text === 'No video from Old hub.', late.old.text);
        check('Bambu A1/P1 tile: access code / network / camera text', late.a1.shown && /access code/.test(late.a1.text) && /same|this network/.test(late.a1.text) && /camera is not disabled/.test(late.a1.text), late.a1.text);
        check('a Bambu A1/P1 tile that has frames shows nothing', !late.a1ok.shown, JSON.stringify(late.a1ok));
        check('every hint has a Retry', ['x1', 'cam', 'old', 'a1'].every(k => late[k].retry));

        // The wording the page shows: light and dark, the text stays readable (tiles are black in both).
        const contrast = async scheme => {
            await pg.S('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: scheme }] });
            await pg.ev(`document.documentElement.setAttribute('data-theme', '${scheme}'); 1`);
            return pg.ev(`(function(){
                var ov = document.querySelector('#grid .cell[data-id="x1"] .nopic'), cs = getComputedStyle(ov);
                function rgb(s) { var m = s.match(/[\\d.]+/g).map(Number); return { r: m[0], g: m[1], b: m[2], a: m.length > 3 ? m[3] : 1 }; }
                function lum(c) { function f(v) { v /= 255; return v <= .03928 ? v / 12.92 : Math.pow((v + .055) / 1.055, 2.4); } return .2126 * f(c.r) + .7152 * f(c.g) + .0722 * f(c.b); }
                var fg = rgb(cs.color), bg = rgb(cs.backgroundColor);
                var base = { r: 0, g: 0, b: 0 };                       // the tile is black
                var comp = { r: bg.r * bg.a + base.r * (1 - bg.a), g: bg.g * bg.a + base.g * (1 - bg.a), b: bg.b * bg.a + base.b * (1 - bg.a) };
                var a = lum(fg) + .05, b = lum(comp) + .05;
                var btn = ov.querySelector('button'), bs = getComputedStyle(btn), bf = rgb(bs.color), bb = rgb(bs.backgroundColor);
                var c2 = (Math.max(lum(bf), lum(bb)) + .05) / (Math.min(lum(bf), lum(bb)) + .05);
                return JSON.stringify({ text: Math.max(a, b) / Math.min(a, b), button: c2, display: cs.display });
            })()`).then(JSON.parse);
        };
        for (const scheme of ['dark', 'light']) {
            const c = await contrast(scheme);
            check('theme ' + scheme + ': the hint text has at least 7:1 contrast', c.text >= 7, c.text);
            check('theme ' + scheme + ': the Retry button label has at least 4.5:1 contrast', c.button >= 4.5, c.button);
        }
        const fits = await pg.ev(`(function(){ var ov = document.querySelector('#grid .cell[data-id="x1"] .nopic'); return JSON.stringify({ sw: ov.scrollWidth, cw: ov.clientWidth, docw: document.documentElement.scrollWidth, vw: innerWidth }); })()`).then(JSON.parse);
        check('phone width: the hint does not overflow the tile or the page', fits.sw <= fits.cw + 1 && fits.docw <= fits.vw + 1, JSON.stringify(fits));

        // A picture arriving removes a hint (the player says so through the frame itself).
        await pg.ev(`(function(){ var f = document.querySelector('#grid .cell[data-id="cam"] iframe');
            f.contentWindow.eval("parent.postMessage({ snorcaPicture: true, src: 'x' }, '*')"); return 1; })()`);
        await sleep(300);
        const afterPic = byId(await cells());
        check('a picture arriving clears that tile\'s hint', !afterPic.cam.shown);
        check('and leaves the other tiles\' hints alone', afterPic.x1.shown && afterPic.a1.shown);

        // Retry on the Bambu picture tile asks the relay again and hides the hint.
        const before = bambuIds.length;
        await pg.ev(`document.querySelector('#grid .cell[data-id="a1"] .nopic button').click(); 1`);
        await sleep(400);
        const rc = byId(await cells());
        check('Retry hides the hint at once', !rc.a1.shown);
        check('Retry asks the relay again', bambuIds.length > before && /[?&]n=1/.test(bambuIds[bambuIds.length - 1]), bambuIds.slice(before).join(' '));
        await sleep(NOPICMS + 1200);
        check('and the hint comes back if there is still no video', byId(await cells()).a1.shown);

        // Retry on a hub-player tile reloads its frame (a new WebSocket), and the hint waits for a new window.
        await pg.ev(`document.querySelector('#grid .cell[data-id="x1"] .nopic button').click(); 1`);
        await sleep(400);
        check('Retry on a frame tile hides the hint at once', !byId(await cells()).x1.shown);
        await sleep(NOPICMS + 1500);
        check('and it returns, with the reason again, if the camera still gives nothing', byId(await cells()).x1.shown && /^Connection refused\./.test(byId(await cells()).x1.text));

        // A hidden page parks the tiles; the clock must not run, so no hint appears while hidden.
        await pg.S('Page.navigate', { url: 'http://127.0.0.1:' + port + BASE + '?nopicms=' + NOPICMS });
        await sleep(800);
        const hide = on => pg.ev(`(function(on){
            Object.defineProperty(document, 'hidden', { configurable: true, get: function() { return on; } });
            Object.defineProperty(document, 'visibilityState', { configurable: true, get: function() { return on ? 'hidden' : 'visible'; } });
            document.dispatchEvent(new Event('visibilitychange')); return 1; })(${on})`);
        await hide(true);
        await sleep(NOPICMS + 1500);
        check('a hidden page shows no hint (the clock stops with the tiles)', Object.values(byId(await cells())).every(c => !c.shown), JSON.stringify(await cells()));
        await hide(false);
        await sleep(1000);
        check('shown again: a fresh start, still no hint in the first second', Object.values(byId(await cells())).every(c => !c.shown));
        await sleep(NOPICMS + 1500);
        check('and the hints come after a full window of running', byId(await cells()).x1.shown && byId(await cells()).a1.shown);
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

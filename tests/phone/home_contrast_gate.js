// Gate: the Home tab's accent colours read at WCAG AA (resources/web/home: home.css, home.js).
//
// A plain Node http server (loopback, no dependencies) serves the real shipped page; a headless
// Edge / Chrome drives it over the DevTools protocol (Node 22's built-in WebSocket). A stand-in for
// the slicer (window.wx) only records; the test sends the 'init' message HomePanel.cpp sends. No
// slicer, no network beyond loopback, nothing touches the desktop.
//
// For the light and the dark page, and for each bundled theme pack (resources/themes/*/theme.json,
// turned into the CSS variables ThemePack::home_css sends), it measures text against its real
// background (the first opaque one up the tree) for: .btn.primary, .rail-item.active, .chip.active,
// .badge.accent and .status button.link. Each must reach 4.5:1. The brand --accent itself stays
// #009688 / #26a69a without a theme (it still draws borders and focus rings).
//
// Run: node tests/phone/home_contrast_gate.js     (skips, exit 0, when no Chromium is found)
// WEB_DIR=<dir> points it at another copy of the page. SHOT_DIR=<dir> saves screenshots there.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
const WEB = process.env.WEB_DIR || path.join(ROOT, 'resources', 'web', 'home');
const THEMES = path.join(ROOT, 'resources', 'themes');

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

const SHIM = `(function(){ window.__sent = [];
    window.wx = { postMessage: function(m) { try { window.__sent.push(JSON.parse(m)); } catch (e) { window.__sent.push({ bad: m }); } } };
})();`;

// ThemePack::home_css: palette roles first, then the pack's own "home" entries (page variables only).
const FROM_PALETTE = [
    ['--bg', 'panel_bg'], ['--rail', 'window_bg'], ['--card', 'window_bg'], ['--ctl', 'window_bg'], ['--thumb', 'panel_bg'],
    ['--fg', 'text'], ['--mute', 'text_secondary'], ['--faint', 'text_disabled'], ['--line', 'separator'], ['--ctl-line', 'border'],
    ['--hover', 'accent_soft'], ['--accent', 'accent'], ['--accent-fg', 'accent_text'], ['--accent-soft', 'accent_soft'], ['--danger', 'error'],
];
const PAGE_VARS = new Set(['--bg', '--rail', '--card', '--fg', '--mute', '--faint', '--line', '--ctl', '--ctl-line', '--hover', '--thumb',
    '--accent', '--accent-fg', '--accent-soft', '--accent-strong', '--accent-text', '--warn-bg', '--warn-fg', '--danger']);
function homeCss(pack) {
    const css = {};
    for (const [v, role] of FROM_PALETTE) if (pack.palette && pack.palette[role]) css[v] = pack.palette[role];
    for (const [v, value] of Object.entries(pack.home || {})) if (PAGE_VARS.has(v)) css[v] = value;
    return css;
}

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

// In the page: text colour and effective background of each accent element. Elements the page only
// builds with data (active chips, badges, the status link) are added as probes inside #main.
const MEASURE = `(function(){
    var main = document.getElementById('main');
    var probe = document.createElement('div');
    probe.id = '__probe';
    probe.innerHTML = '<div class="chips"><button class="chip active">Chip</button></div>' +
        '<span class="badge accent">Badge</span><div class="status">Text<button class="link">Link</button></div>';
    var old = document.getElementById('__probe'); if (old) old.remove();
    main.appendChild(probe);
    function back(e) {
        for (; e; e = e.parentElement) {
            var c = getComputedStyle(e).backgroundColor;
            if (c && c !== 'transparent' && !/rgba\\([^)]*,\\s*0\\)$/.test(c)) return c;
        }
        return 'rgb(255, 255, 255)';
    }
    var out = {};
    var pick = { '.btn.primary': '#new-project', '.rail-item.active': '.rail-item.active', '.chip.active': '#__probe .chip.active',
        '.badge.accent': '#__probe .badge.accent', '.status button.link': '#__probe .status button.link' };
    for (var k in pick) {
        var e = document.querySelector(pick[k]);
        out[k] = e ? { fg: getComputedStyle(e).color, bg: back(e) } : null;
    }
    var root = getComputedStyle(document.documentElement);
    out.__accent = root.getPropertyValue('--accent').trim().toLowerCase();
    out.__dark = document.documentElement.classList.contains('dark');
    return out;
})()`;

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const dbgPort = 9700 + Math.floor(Math.random() * 200);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'homecontrast-'));
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
            const r = (await S('Runtime.evaluate', { expression: expr, returnByValue: true })).result;
            if (r.exceptionDetails) throw new Error('page eval failed: ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text));
            return r.result.value;
        };
        await S('Page.enable'); await S('Runtime.enable');
        await S('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });
        await S('Emulation.setDeviceMetricsOverride', { width: 1200, height: 800, deviceScaleFactor: 1, mobile: false });

        // The slicer marks its theme in the User-Agent; home.js adds html.dark from it.
        const baseUa = ver['User-Agent'] || 'Mozilla/5.0';
        const scenarios = [{ name: 'light', dark: false, theme: null }, { name: 'dark', dark: true, theme: null }];
        for (const dir of fs.existsSync(THEMES) ? fs.readdirSync(THEMES).sort() : []) {
            const f = path.join(THEMES, dir, 'theme.json');
            if (!fs.existsSync(f)) continue;
            const pack = JSON.parse(fs.readFileSync(f, 'utf8'));
            scenarios.push({ name: 'theme ' + dir, dark: pack.base === 'dark', theme: homeCss(pack) });
        }

        for (const sc of scenarios) {
            await S('Emulation.setUserAgentOverride', { userAgent: baseUa + (sc.dark ? ' EdgeSlicer/theme-dark' : ' EdgeSlicer/theme-light') });
            const errBefore = cdp.events.filter(d => d.sessionId === sessionId && d.method === 'Runtime.exceptionThrown').length;
            await S('Page.navigate', { url: 'http://127.0.0.1:' + port + '/home/index.html' });
            for (let i = 0; i < 50; i++) { if (await evalJs('!!(window.HomeApp && document.readyState === "complete")').catch(() => false)) break; await sleep(100); }
            const init = { type: 'init', strings: {}, section: 'recent', models: false };
            if (sc.theme) init.theme = sc.theme;
            await evalJs('window.HomeApp.receive(' + JSON.stringify(init) + '), true');
            await sleep(100);
            const errs = cdp.events.filter(d => d.sessionId === sessionId && d.method === 'Runtime.exceptionThrown').length - errBefore;
            check(sc.name + ': no uncaught exception', errs === 0, errs);
            const m = await evalJs(MEASURE);
            check(sc.name + ': page is ' + (sc.dark ? 'dark' : 'light'), m.__dark === sc.dark);
            if (!sc.theme)
                check(sc.name + ': brand --accent kept for borders', m.__accent === (sc.dark ? '#26a69a' : '#009688'), m.__accent);
            for (const k of ['.btn.primary', '.rail-item.active', '.chip.active', '.badge.accent', '.status button.link']) {
                const v = m[k];
                if (!v) { check(sc.name + ': ' + k + ' present', false); continue; }
                const fg = parseColor(v.fg), bg = parseColor(v.bg);
                const r = fg && bg ? contrast(fg, bg) : 0;
                check(sc.name + ': ' + k + ' contrast ' + r.toFixed(2) + ':1 >= 4.5', r >= 4.5, v.fg + ' on ' + v.bg);
            }
            if (process.env.SHOT_DIR) {
                const shot = (await S('Page.captureScreenshot', { format: 'png' })).result.data;
                fs.mkdirSync(process.env.SHOT_DIR, { recursive: true });
                fs.writeFileSync(path.join(process.env.SHOT_DIR, 'home_' + sc.name.replace(/\W+/g, '_') + '.png'), Buffer.from(shot, 'base64'));
            }
        }
    } finally {
        try { br.kill(); } catch (e) { }
        server.close();
        await sleep(300);
        try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { }
    }
    console.log(`\n${passed} passed, ${failures} failed`);
    return failures ? 1 : 0;
}

main().then(code => process.exit(code), e => { console.error(e); process.exit(2); });

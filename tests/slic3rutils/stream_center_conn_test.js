// Headless regression check for the hub page's Home/Away connection switch
// (resources/web/orca/stream_center.html, the "connSwitch / connMaybeHop" block).
//
// This is plain Node with no dependencies and no browser: it slices the real shipped
// <script> source between the `CONN_KEY` declaration and the end of `connHint()`, and
// evaluates that slice in a small sandbox with mocked location/localStorage/sessionStorage/
// history. That keeps the test honest against the actual file (not a reimplementation)
// while staying decoupled from the rest of the page's IIFE, which needs a full DOM.
//
// Regression covered: aceRage/EdgeSlicerApp#7 item 3 - "Switch to remote" bouncing straight
// back to Home ~1.5s later, because the preference written by connSwitch() lives in the
// *origin being left*, not the one being arrived at (localStorage does not cross origins),
// so connMaybeHop() on arrival still saw the stale/absent preference and re-probed home.
//
// Run: node tests/slic3rutils/stream_center_conn_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML_PATH = path.join(__dirname, '..', '..', 'resources', 'web', 'orca', 'stream_center.html');
const html = fs.readFileSync(HTML_PATH, 'utf8');

const START = /var CONN_KEY\s*=/;
const END_MARKER = 'function connHint(node) {';

function extractConnBlock(source) {
    const startMatch = START.exec(source);
    if (!startMatch) throw new Error('connSwitch block start marker not found - file structure changed');
    const endIdx = source.indexOf(END_MARKER, startMatch.index);
    if (endIdx === -1) throw new Error('connHint marker not found - file structure changed');
    // Walk forward from connHint's opening brace to its matching closing brace.
    let i = source.indexOf('{', endIdx);
    let depth = 0, end = -1;
    for (; i < source.length; i++) {
        if (source[i] === '{') depth++;
        else if (source[i] === '}') { depth--; if (depth === 0) { end = i + 1; break; } }
    }
    if (end === -1) throw new Error('could not find end of connHint()');
    return source.slice(startMatch.index, end);
}

const connBlock = extractConnBlock(html);

let failures = 0, passed = 0;
function check(label, cond) {
    if (cond) { passed++; }
    else { failures++; console.error('FAIL: ' + label); }
}

// ---- A tiny per-origin store, so "localStorage" behaves like the real thing: separate jars
// per origin, which is the whole reason the bug exists. -------------------------------------
function makeOriginStore() {
    const stores = {};
    return function forOrigin(origin) {
        if (!stores[origin]) stores[origin] = {};
        const s = stores[origin];
        return {
            getItem: (k) => (k in s ? s[k] : null),
            setItem: (k, v) => { s[k] = String(v); },
            removeItem: (k) => { delete s[k]; },
        };
    };
}
const localStores = makeOriginStore();
const sessionStores = makeOriginStore();

// Simulates one page load at `url`, running the extracted connection-logic source in a fresh
// sandbox whose location/localStorage/sessionStorage reflect that origin. Returns a handle to
// poke at connSwitch/connMaybeHop and to read back the resulting state.
function loadPage(url, remoteInfo) {
    const u = new URL(url);
    const origin = u.origin;
    const sandbox = {
        REMOTE: '/r/tok123/',
        location: {
            protocol: u.protocol,
            pathname: u.pathname,
            search: u.search,
            hash: u.hash,
            host: u.host,
            href: u.href,
        },
        localStorage: localStores(origin),
        sessionStorage: sessionStores(origin),
        history: {
            replaceState: function(_state, _title, newUrl) {
                // newUrl is pathname+search+hash here (no origin), matching real usage.
                const parsed = new URL(newUrl, u.origin);
                sandbox.location.pathname = parsed.pathname;
                sandbox.location.search = parsed.search;
                sandbox.location.hash = parsed.hash;
                sandbox.location.href = parsed.href;
            },
        },
        window: {}, // no caches -> connSetPref's Cache API mirror is skipped, same as a browser without it
        URL: URL,
        console: console,
        navigated: null, // captures the last location.href the tested code "navigated" to
    };
    // location.href is a plain string in this mock; connSwitch assigns to it to navigate. We
    // intercept that by making location an object with a setter-tracked href.
    let hrefValue = u.href;
    Object.defineProperty(sandbox.location, 'href', {
        get() { return hrefValue; },
        set(v) { hrefValue = v; sandbox.navigated = v; },
    });
    vm.createContext(sandbox);
    vm.runInContext(connBlock, sandbox);
    // connLan/connRemote are populated from /state in the real page (fetchRemoteState); mimic
    // that assignment, which happens before connRender()/connMaybeHop() on every load.
    vm.runInContext(
        'connLan = ' + JSON.stringify(remoteInfo.lan_url) + ';' +
        'connRemote = ' + JSON.stringify(remoteInfo.remote_url) + ';' +
        "if (connRemote && connOrigin(connRemote) === " + JSON.stringify(origin) + ") connHere = 'away';" +
        "else if (connLan && connOrigin(connLan) === " + JSON.stringify(origin) + ") connHere = 'home';",
        sandbox
    );
    return sandbox;
}

const LAN_URL = 'http://192.168.1.50:13640/r/tok123/';
const REMOTE_URL = 'https://pc-abcd.tailnet.ts.net/r/tok123/';

// ---- Test 1: connSwitch('away') carries the choice in the URL hash. ------------------------
{
    const page = loadPage(LAN_URL, { lan_url: LAN_URL, remote_url: REMOTE_URL });
    vm.runInContext("connSwitch('away');", page);
    check('connSwitch(away) navigates to the remote origin', (page.navigated || '').indexOf('https://pc-abcd.tailnet.ts.net') === 0);
    check('connSwitch(away) carries #conn=away in the URL', /#conn=away$/.test(page.navigated || ''));
}

// ---- Test 2: arriving with #conn=away saves the preference locally and skips the hop -------
// even though this origin's own stored preference is the stale 'home' left behind by an
// earlier "Switch to home network" - this is the exact repro from EdgeSlicerApp#7 item 3.
{
    localStores(new URL(REMOTE_URL).origin).setItem('snorca_conn_pref', 'home'); // stale leftover
    const page = loadPage(REMOTE_URL + '#conn=away', { lan_url: LAN_URL, remote_url: REMOTE_URL });
    let hopped = false;
    vm.runInContext('connSwitch = function(to) { globalThis.__hopped = to; };', page); // stub out the actual navigation
    page.__hopped = null;
    vm.runInContext('connMaybeHop();', page);
    hopped = page.__hopped;
    check('preference is saved as "away" on the remote origin on arrival', page.connPref() === 'away');
    check('the #conn= marker is stripped from the URL', page.location.hash === '');
    check('connMaybeHop does not bounce back to home on the load that carried #conn=away', hopped === null);
}

// ---- Test 3: a normal load with no marker still auto-hops home when the LAN is reachable ---
// (the existing, wanted behaviour must survive the fix).
{
    localStores(new URL(REMOTE_URL).origin).setItem('snorca_conn_pref', 'home');
    sessionStores(new URL(REMOTE_URL).origin).removeItem('snorca_conn_tried');
    const page = loadPage(REMOTE_URL, { lan_url: LAN_URL, remote_url: REMOTE_URL });
    let reachedProbe = false;
    vm.runInContext('connProbe = function(cb) { globalThis.__reachedProbe = true; };', page);
    page.__reachedProbe = false;
    vm.runInContext('connMaybeHop();', page);
    reachedProbe = page.__reachedProbe;
    check('an unmarked load with a "home" preference still attempts the automatic hop', reachedProbe === true);
}

// ---- Test 4: connSkipHop is consumed by the first connMaybeHop() call, not stuck "true"
// forever - if the preference later legitimately becomes 'home' again in the same load (the
// page polls /state every 15s), the automatic hop must still be able to fire.
{
    localStores(new URL(REMOTE_URL).origin).setItem('snorca_conn_pref', 'away');
    sessionStores(new URL(REMOTE_URL).origin).removeItem('snorca_conn_tried'); // undo test 3's side effect
    const page = loadPage(REMOTE_URL + '#conn=away', { lan_url: LAN_URL, remote_url: REMOTE_URL });
    vm.runInContext('connProbe = function(cb) { globalThis.__reachedProbe = (globalThis.__reachedProbe||0)+1; };', page);
    page.__reachedProbe = 0;
    vm.runInContext('connMaybeHop();', page); // consumes connSkipHop; pref is 'away' so no probe anyway
    check('marker-carrying load does not probe', page.__reachedProbe === 0);
    vm.runInContext("localStorage.setItem('snorca_conn_pref', 'home'); connMaybeHop();", page); // later poll flips the pref
    check('a later poll in the same load can still hop once the marker is consumed', page.__reachedProbe === 1);
}

console.log(passed + ' passed, ' + failures + ' failed');
process.exit(failures ? 1 : 0);

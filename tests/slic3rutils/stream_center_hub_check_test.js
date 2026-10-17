// Headless regression check for the Stream tab's hub handshake
// (resources/web/orca/stream_center.html: __hubReady, hubCheck and their backoff).
//
// Owner, 2026-10-16: after the hub restarted (another EdgeSlicer install took it over, which gives
// it a new per-run secret on the same port), every Bambu tile in the PC's Stream tab showed the
// hub's plain-text "not found" - the tiles still carried the old secret - and only restarting the
// slicer brought them back. The U1 tiles, which talk to the printers directly, kept playing.
//
// Plain Node, no dependencies, no browser: like stream_center_conn_test.js it slices the real
// shipped <script> source (from `var HUB_CHECK_MIN_MS` to the end of dropHubTiles()) and runs it
// in a sandbox with mocked render/postToSlicer/timers/DOM queries.
//
// Run: node tests/slic3rutils/stream_center_hub_check_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HTML_PATH = path.join(__dirname, '..', '..', 'resources', 'web', 'orca', 'stream_center.html');
const html = fs.readFileSync(HTML_PATH, 'utf8');

function sliceThroughFunction(source, startRe, fnHead) {
    const m = startRe.exec(source);
    if (!m) throw new Error('start marker not found - file structure changed');
    const at = source.indexOf(fnHead, m.index);
    if (at === -1) throw new Error(fnHead + ' not found - file structure changed');
    let i = source.indexOf('{', at), depth = 0, end = -1;
    for (; i < source.length; i++) {
        if (source[i] === '{') depth++;
        else if (source[i] === '}') { depth--; if (depth === 0) { end = i + 1; break; } }
    }
    if (end === -1) throw new Error('unbalanced ' + fnHead);
    return source.slice(m.index, end);
}

const block = sliceThroughFunction(html, /var HUB_CHECK_MIN_MS\s*=/, 'function dropHubTiles() {');

let failures = 0, passed = 0;
function check(label, cond) {
    if (cond) passed++;
    else { failures++; console.error('FAIL: ' + label); }
}

function loadPage(opts) {
    opts = opts || {};
    const calls = { render: 0, posts: [], pushState: 0 };
    const timers = [];
    const hubCells = opts.hubCells || [{ dataset: { id: 'x1c', hub: '1' } }, { dataset: { id: 'h2d', hub: '1' } }];
    const sandbox = {
        REMOTE: opts.remote ? '/r/tok/' : '',
        relayPort: 0,
        window: {},
        render: () => { calls.render++; },
        pushState: () => { calls.pushState++; },
        hashStr: (s) => String(s.length),
        postToSlicer: (m) => { calls.posts.push(m); },
        tilesShowing: () => opts.showing !== false,
        setTimeout: (fn, ms) => { timers.push({ fn, ms, live: true }); return timers.length; },
        clearTimeout: (id) => { if (id > 0 && timers[id - 1]) timers[id - 1].live = false; },
        document: {
            querySelector: (sel) => (sel.indexOf('[data-hub]') !== -1 && hubCells.length ? hubCells[0] : null),
            querySelectorAll: (sel) => (sel.indexOf('[data-hub]') !== -1 ? hubCells : []),
            createElement: () => ({ dataset: {}, appendChild() {} }),
        },
    };
    vm.createContext(sandbox);
    vm.runInContext(block + '\nthis.__t = { get ms() { return hubCheckMs; }, get secret() { return hubSecret; }, get port() { return go2rtcPort; },' +
                    ' get relay() { return relayPort; }, get streams() { return go2rtcStreams; }, set streams(v) { go2rtcStreams = v; },' +
                    ' ensureGo2rtc: ensureGo2rtc, hubCheck: hubCheck, hubNextCheckMs: hubNextCheckMs, HUB_CHECK_MIN_MS: HUB_CHECK_MIN_MS, HUB_CHECK_MAX_MS: HUB_CHECK_MAX_MS };',
                    sandbox);
    const live = () => timers.filter((t) => t.live);
    const fire = () => { const t = live().pop(); if (!t) return null; t.live = false; t.fn(); return t.ms; };
    return { t: sandbox.__t, w: sandbox.window, calls, timers, live, fire, hubCells };
}

// ---- backoff ---------------------------------------------------------------------------------
{
    const p = loadPage();
    const t = p.t;
    check('first wait is the minimum', t.hubNextCheckMs(0, false) === t.HUB_CHECK_MIN_MS);
    let ms = 0, seq = [];
    for (let i = 0; i < 7; i++) { ms = t.hubNextCheckMs(ms, false); seq.push(ms); }
    check('waits double up to the heartbeat: ' + seq.join(','), seq.join(',') === '2000,4000,8000,16000,30000,30000,30000');
    check('a change goes back to the minimum', t.hubNextCheckMs(30000, true) === t.HUB_CHECK_MIN_MS);
}

// ---- the regression: a hub restart with a new secret rebuilds the hub tiles ------------------
{
    const p = loadPage();
    p.t.ensureGo2rtc();
    check('the page asks for the hub once', p.calls.posts.length === 1 && p.calls.posts[0] === 'hub_start');
    p.w.__hubReady(13640, 5000, 'secret-a');
    check('first answer renders', p.calls.render === 1);
    check('first answer keeps the tiles', p.hubCells.every((c) => c.dataset.id !== ''));
    check('a check is scheduled after the first answer', p.live().length === 1 && p.live()[0].ms === 2000);

    // Same hub, answered again: nothing redrawn, the check backs off.
    p.fire();
    check('the scheduled check asks the slicer', p.calls.posts.length === 2);
    p.w.__hubReady(13640, 5000, 'secret-a');
    check('an unchanged hub does not redraw', p.calls.render === 1);
    check('an unchanged hub backs off', p.t.ms === 4000);

    // The hub restarted: same port, new secret (the owner's case).
    p.t.streams = { s123: true };
    p.fire();
    p.w.__hubReady(13640, 5001, 'secret-b');
    check('a new secret redraws', p.calls.render === 2);
    check('the hub tiles are dropped so render() rebuilds them with the new secret', p.hubCells.every((c) => c.dataset.id === ''));
    check('the page holds the new secret', p.t.secret === 'secret-b' && p.t.relay === 5001);
    check('streams are pushed again from the rebuilt tiles', Object.keys(p.t.streams).length === 0);
    check('the check goes back to the minimum wait', p.t.ms === 2000);
}

// ---- a relay that failed to start comes back by itself ---------------------------------------
{
    const p = loadPage();
    p.t.ensureGo2rtc();
    p.w.__hubReady(0, 0, '');
    check('a failed hub still schedules a retry', p.live().length === 1);
    p.fire();
    check('the retry asks the slicer again', p.calls.posts.length === 2);
    p.w.__hubReady(13640, 5000, 'secret-a');
    check('the hub coming up redraws the failed tiles', p.calls.render === 2 && p.hubCells.every((c) => c.dataset.id === ''));
}

// ---- Retry / tab shown again: ask at once, from the minimum ------------------------------------
{
    const p = loadPage();
    p.t.ensureGo2rtc();
    p.w.__hubReady(13640, 5000, 'a');
    for (let i = 0; i < 4; i++) { p.fire(); p.w.__hubReady(13640, 5000, 'a'); }
    check('backed off before the retry', p.t.ms === 30000);
    const before = p.calls.posts.length;
    p.t.hubCheck(true);
    check('Retry asks at once', p.calls.posts.length === before + 1);
    check('Retry resets the wait', p.t.ms === 2000);
}

// ---- nothing to check: no hub tile on screen, a hidden tab, or the phone page ------------------
{
    const hidden = loadPage({ showing: false });
    hidden.t.ensureGo2rtc();
    hidden.w.__hubReady(13640, 5000, 'a');
    const n = hidden.calls.posts.length;
    hidden.fire();
    check('a hidden tab does not ask', hidden.calls.posts.length === n);
    check('but keeps a check scheduled', hidden.live().length === 1);

    const empty = loadPage({ hubCells: [] });
    empty.t.ensureGo2rtc();
    empty.w.__hubReady(13640, 5000, 'a');
    const m = empty.calls.posts.length;
    empty.fire();
    check('no hub tile: no ask', empty.calls.posts.length === m);

    const phone = loadPage({ remote: true });
    phone.w.__hubReady(13640, 5000, 'a');
    check('the phone page schedules nothing', phone.live().length === 0);
    phone.t.hubCheck(true);
    check('the phone page never asks the slicer', phone.calls.posts.length === 0);

    const never = loadPage();
    never.t.hubCheck(true);
    check('no check before the page has asked for a hub at all', never.calls.posts.length === 0);
}

console.log(passed + ' passed, ' + failures + ' failed');
process.exit(failures ? 1 : 0);

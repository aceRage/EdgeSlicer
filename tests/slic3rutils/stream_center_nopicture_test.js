// Headless check for the Stream tab's "No video" hint
// (resources/web/orca/stream_center.html, the NOPIC block).
//
// What it pins:
//   * the wording per camera type: Bambu X1/H2 (rtsps) names LAN Mode Liveview and the access code,
//     Bambu A1/P1 (port-6000 picture stream) names the access code / network / camera setting, every
//     other type says "No video from <host>" with a reason only when the hub reported one;
//   * a camera's host is shown without its login, port or path (a pasted rtsp://user:pass@... URL);
//   * an <img> tile: no hint inside the window, the hint after it, none once a picture loads,
//     Retry hides the hint and tries again, a parked tile never shows one, a started tile restarts
//     the clock;
//   * a hub player's snorcaPicture / snorcaNoPicture reach only the tile whose frame sent them.
//
// Plain Node, no dependencies, no browser: it slices the real shipped <script> source and runs it
// in a sandbox with fake timers.
//
// Run: node tests/slic3rutils/stream_center_nopicture_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'resources', 'web', 'orca', 'stream_center.html'), 'utf8');
const from = html.indexOf('// ---- "No video" hint');
const to = html.indexOf('// ---- end NOPIC block', from);
if (from === -1 || to === -1) throw new Error('NOPIC block not found - file structure changed');
const block = html.slice(from, to);

let failures = 0, passed = 0;
function check(label, cond, extra) {
    if (cond) passed++;
    else { failures++; console.error('FAIL: ' + label + (extra !== undefined ? '  (' + extra + ')' : '')); }
}

function fakeElement(tag) {
    const listeners = {};
    const attrs = {};
    const el = {
        tagName: tag.toUpperCase(), style: {}, children: [], textContent: '', dataset: {}, className: '',
        setAttribute(k, v) { attrs[k] = String(v); },
        getAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k) ? attrs[k] : null; },
        removeAttribute(k) { delete attrs[k]; },
        appendChild(c) { el.children.push(c); return c; },
        addEventListener(ev, fn) { listeners[ev] = fn; },
        fire(ev) { if (listeners[ev]) listeners[ev](); },
        querySelector(sel) { return el.children.filter((c) => c.tagName === sel.toUpperCase())[0] || null; },
    };
    return el;
}

// Fake timers: setTimeout/clearTimeout driven by advance(ms).
function makeClock() {
    let now = 0, nextId = 1;
    const timers = new Map();
    return {
        setTimeout(fn, ms) { const id = nextId++; timers.set(id, { at: now + ms, fn }); return id; },
        clearTimeout(id) { timers.delete(id); },
        advance(ms) {
            const end = now + ms;
            for (;;) {
                let best = null;
                timers.forEach((t, id) => { if (t.at <= end && (!best || t.at < best.t.at)) best = { id, t }; });
                if (!best) break;
                timers.delete(best.id); now = best.t.at; best.t.fn();
            }
            now = end;
        },
    };
}

function load(search) {
    const clock = makeClock();
    const cellsList = [];
    const sandbox = {
        location: { search: search || '' },
        URLSearchParams,
        document: { createElement: fakeElement, querySelectorAll: () => cellsList },
        setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    };
    vm.createContext(sandbox);
    vm.runInContext(block + '\nthis.__t = { NOPIC_MS, NOPIC_QS, noPictureText, camHostOnly, noPicAttachImg, noPicAttachFrame, noPicOnMessage };', sandbox);
    return { t: sandbox.__t, clock, cellsList };
}

(() => {
    const { t, clock, cellsList } = load();
    check('the wait is 10 s by default', t.NOPIC_MS === 10000 && t.NOPIC_QS === '');
    check('?nopicms shortens it (and is handed to the player)', (() => { const o = load('?nopicms=1500').t; return o.NOPIC_MS === 1500 && o.NOPIC_QS === '&nopicms=1500'; })());
    check('a silly ?nopicms is ignored', load('?nopicms=5').t.NOPIC_MS === 10000);

    // ---- wording -----------------------------------------------------------------------------
    check('Bambu X1/H2 text', t.noPictureText('rtsps', '', 'x') ===
        'No video. Check that LAN Mode Liveview is on at the printer (Settings on the printer’s screen), and that the access code is current.');
    check('Bambu X1/H2 with a real reason', t.noPictureText('rtsps', 'refused', 'x').indexOf('Connection refused. Check that LAN Mode Liveview') === 0);
    check('Bambu X1/H2 ignores a reason it does not know', t.noPictureText('rtsps', 'nonsense', 'x').indexOf('No video. Check that LAN') === 0);
    check('Bambu A1/P1 text names the code, the network and the camera setting',
        /access code/.test(t.noPictureText('p1', '', 'x')) && /network/.test(t.noPictureText('p1', '', 'x')) && /camera is not disabled/.test(t.noPictureText('p1', '', 'x')));
    check('Bambu A1/P1 does not claim a LAN Liveview setting', !/Liveview/.test(t.noPictureText('p1', '', 'x')));
    check('generic text without a reason', t.noPictureText('ipcam', '', '192.168.1.9') === 'No video from 192.168.1.9.');
    check('generic text with the reason go2rtc gave', t.noPictureText('ipcam', 'auth', 'Garage') === 'No video from Garage. Login refused.');
    check('every reason has words', ['auth', 'refused', 'unreachable', 'timeout', 'codec', 'ffmpeg'].every((r) => /\w/.test(t.noPictureText('wyze', r, 'h').split('. ')[1] || '')));
    check('a generic tile with no name still reads', t.noPictureText('', '', '') === 'No video from the camera.');

    check('host only: bare address', t.camHostOnly('192.168.1.5') === '192.168.1.5');
    check('host only: strips login, port and path', t.camHostOnly('rtsp://admin:secret@10.0.0.7:554/stream1?x=1') === '10.0.0.7');
    check('host only: onvif url', t.camHostOnly('onvif://u:p@cam.local:80?subtype=0') === 'cam.local');
    check('host only: no password survives', t.camHostOnly('rtsps://bblp:12345678@192.168.1.2:322/streaming/live/1').indexOf('12345678') === -1);

    // ---- an <img> tile -----------------------------------------------------------------------
    const mk = () => {
        const cell = fakeElement('div'), im = fakeElement('img');
        im.setAttribute('src', 'http://127.0.0.1:1/bambu?ip=1.2.3.4&code=abc');
        cell.appendChild(im);
        t.noPicAttachImg(cell, im, { cam: 'p1', name: 'P1' });
        return { cell, im, ov: cell.children[1] };
    };
    {
        const a = mk();
        check('the overlay is built hidden', a.ov.style.display === undefined || a.ov.style.display === 'none' || a.ov.style.display === '');
        a.cell._npwStart();
        clock.advance(9999);
        check('no hint inside the window', a.ov.style.display !== 'flex');
        clock.advance(2);
        check('the hint shows once the window has passed', a.ov.style.display === 'flex');
        check('the hint text is the A1/P1 one', /access code/.test(a.ov.children[0].textContent));
        a.im.fire('load');
        check('a picture clears the hint', a.ov.style.display === 'none');
    }
    {
        const a = mk();
        a.cell._npwStart();
        clock.advance(5000);
        a.im.fire('load');
        clock.advance(60000);
        check('a picture inside the window means no hint, ever', a.ov.style.display !== 'flex');
    }
    {
        const a = mk();
        a.cell._npwStart();
        clock.advance(11000);
        const retry = a.ov.children[1];
        retry.onclick({ stopPropagation() {} });
        check('Retry hides the hint', a.ov.style.display === 'none');
        check('Retry asks the relay again (cache-busted source)', /&n=1$/.test(a.im.getAttribute('src')) && a.im.getAttribute('src').indexOf('code=abc') !== -1, a.im.getAttribute('src'));
        clock.advance(11000);
        check('and the hint returns if that fails too', a.ov.style.display === 'flex');
        retry.onclick({ stopPropagation() {} });
        check('a second Retry replaces the counter instead of stacking', /&n=2$/.test(a.im.getAttribute('src')) && a.im.getAttribute('src').indexOf('&n=1') === -1, a.im.getAttribute('src'));
    }
    {
        const a = mk();
        a.cell._npwStart();
        clock.advance(4000);
        a.cell._npwStop(); // parked: off screen, hidden page
        clock.advance(60000);
        check('a parked tile never shows the hint', a.ov.style.display !== 'flex');
        a.cell._npwStart(); // back on screen: a fresh clock
        clock.advance(9000);
        check('a tile put back gets a fresh window', a.ov.style.display !== 'flex');
        clock.advance(2000);
        check('and the hint after it', a.ov.style.display === 'flex');
    }
    {
        const a = mk();
        a.cell._npwStart();
        a.im.fire('error');
        check('an HTTP error from the relay shows the hint at once', a.ov.style.display === 'flex');
        const b = mk();
        b.im.removeAttribute('src');
        b.cell._npwStart();
        b.im.fire('error');
        check('an error on a parked tile (source taken away) is not reported', b.ov.style.display !== 'flex');
    }

    // ---- a hub player's word for its own tile ---------------------------------------------------
    {
        const win1 = {}, win2 = {};
        const mkFrame = (win, cam, name) => {
            const cell = fakeElement('div'), f = fakeElement('iframe');
            f.contentWindow = win; f.src = 'http://x/stream.html';
            cell.appendChild(f);
            t.noPicAttachFrame(cell, f, { cam, name });
            cell.querySelector = (sel) => sel === 'iframe' ? f : null;
            cellsList.push(cell);
            return { cell, f, ov: cell.children[1] };
        };
        const A = mkFrame(win1, 'rtsps', 'X1'), B = mkFrame(win2, 'ipcam', 'Garage');
        t.noPicOnMessage({ source: win1, data: { snorcaNoPicture: true, reason: 'refused' } });
        check('the frame that reported gets the hint', A.ov.style.display === 'flex' && /^Connection refused\. Check that LAN Mode Liveview/.test(A.ov.children[0].textContent));
        check('another tile is untouched', B.ov.style.display !== 'flex');
        t.noPicOnMessage({ source: win2, data: { snorcaNoPicture: true, reason: '' } });
        check('a generic tile with no reason', B.ov.style.display === 'flex' && B.ov.children[0].textContent === 'No video from Garage.');
        t.noPicOnMessage({ source: win1, data: { snorcaPicture: true } });
        check('snorcaPicture clears the hint', A.ov.style.display === 'none' && B.ov.style.display === 'flex');
        t.noPicOnMessage({ source: {}, data: { snorcaNoPicture: true } });
        t.noPicOnMessage({ source: win1, data: { aspect: 1 } });
        t.noPicOnMessage({ source: win1, data: 'snorcaNoPicture' });
        t.noPicOnMessage(null);
        check('a message from no known frame, or not ours, changes nothing', A.ov.style.display === 'none');
        A.cell._npwStart(); // a parked-and-restored tile starts clean
        B.cell._npwStart();
        check('a restored frame tile clears its hint to start over', B.ov.style.display === 'none');
        const r = B.ov.children[1];
        B.f.src = 'http://x/stream.html?src=q';
        r.onclick({ stopPropagation() {} });
        check('Retry on a frame tile reloads the frame and hides the hint', B.ov.style.display === 'none' && B.f.src === 'http://x/stream.html?src=q');
    }
})();

console.log(passed + ' passed, ' + failures + ' failed');
process.exit(failures ? 1 : 0);

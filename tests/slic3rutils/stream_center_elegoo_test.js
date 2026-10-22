// Headless check for the Stream tab's Elegoo Centauri Carbon (CC1) tile
// (resources/web/orca/stream_center.html: elegooHost, elegooMessage, elegooAttach, applyElegooSeed).
//
// What it pins:
//   * the printer's address is taken out of whatever was typed (scheme, user, port, path);
//   * a tile the relay refused says WHY - "Camera busy: too many viewers (Elegoo allows 4)" for the
//     printer's viewer limit - instead of staying blank, and Retry asks the relay again;
//   * a parked tile (source taken away on purpose) does not report an error;
//   * the printers handed over from Printer settings become tiles once, and one the person
//     removed is not brought back.
//
// Plain Node, no dependencies, no browser: it slices the real shipped <script> source (from
// `var ELEGOO_BUSY` to the Flashforge block that follows) and runs it in a sandbox.
//
// Run: node tests/slic3rutils/stream_center_elegoo_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'resources', 'web', 'orca', 'stream_center.html'), 'utf8');
const from = html.indexOf('var ELEGOO_BUSY');
const to = html.indexOf('// Flashforge (Creator 5 / Adventurer 5M family)', from);
if (from === -1 || to === -1) throw new Error('Elegoo block not found - file structure changed');
const block = html.slice(from, to);

let failures = 0, passed = 0;
function check(label, cond) {
    if (cond) passed++;
    else { failures++; console.error('FAIL: ' + label); }
}

function fakeElement(tag) {
    const listeners = {};
    const attrs = {};
    const el = {
        tagName: tag.toUpperCase(), style: {}, children: [], textContent: '', dataset: {},
        setAttribute(k, v) { attrs[k] = v; },
        getAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k) ? attrs[k] : null; },
        removeAttribute(k) { delete attrs[k]; },
        appendChild(c) { el.children.push(c); return c; },
        addEventListener(ev, fn) { listeners[ev] = fn; },
        fire(ev) { if (listeners[ev]) listeners[ev](); },
    };
    Object.defineProperty(el, 'src', { get() { return attrs.src || ''; }, set(v) { attrs.src = v; } });
    return el;
}

function load(opts) {
    opts = opts || {};
    const sandbox = {
        REMOTE: opts.remote ? '/r/tok/' : '',
        state: { hosts: [], active: [] },
        hubStateKnown: opts.known !== false,
        window: {},
        calls: { save: 0, render: 0, fetched: [] },
        newId: (() => { let n = 0; return () => 'id' + (++n); })(),
        save: () => { sandbox.calls.save++; },
        render: () => { sandbox.calls.render++; },
        document: { createElement: fakeElement },
        fetch(url) {
            sandbox.calls.fetched.push(url);
            if (opts.statusFails) return Promise.reject(new Error('down'));
            return Promise.resolve({ json: () => Promise.resolve(opts.status || { state: 'failed' }) });
        },
    };
    vm.createContext(sandbox);
    vm.runInContext(block + '\nthis.__t = { elegooHost, elegooMessage, elegooAttach, applyElegooSeed, ELEGOO_BUSY };', sandbox);
    return { sandbox, t: sandbox.__t, w: sandbox.window };
}

const tick = () => new Promise((r) => setImmediate(r));

(async () => {
    // ---- the Add dialog calls the type ElegooLink (the id stays 'elegoo' so saved tiles keep working) --
    {
        const m = /\{ value: 'elegoo',\s*label: '([^']*)' \}/.exec(html);
        check('the type is listed as ElegooLink', !!m && m[1] === 'ElegooLink');
        check('the page does not call it Centauri Carbon (CC1) any more', html.indexOf("label: 'Elegoo Centauri Carbon (CC1)'") === -1);
    }

    // ---- the address ---------------------------------------------------------------------------
    {
        const { t } = load();
        check('a bare address stays', t.elegooHost('192.168.1.50') === '192.168.1.50');
        check('scheme, port and path go', t.elegooHost('http://192.168.1.50:3031/video') === '192.168.1.50');
        check('user info goes', t.elegooHost('user@printer.local') === 'printer.local');
        check('empty is empty', t.elegooHost('') === '' && t.elegooHost(null) === '');
    }

    // ---- the words on the tile -----------------------------------------------------------------
    {
        const { t } = load();
        check('busy names the limit', t.elegooMessage({ state: 'busy' }) === 'Camera busy: too many viewers (Elegoo allows 4).');
        check('busy text is the shared constant', t.ELEGOO_BUSY.indexOf('too many viewers (Elegoo allows 4)') !== -1);
        check('no camera says so', /no camera/i.test(t.elegooMessage({ state: 'no_camera' })));
        check('failed names the limit as a hint', /only 4/.test(t.elegooMessage({ state: 'failed' })));
        check('an unreachable relay still gives a sentence', t.elegooMessage(null).length > 20);
    }

    // ---- a refused tile explains itself, Retry asks again ----------------------------------------
    {
        const { sandbox, t } = load({ status: { state: 'busy', message: 'x' } });
        const cell = fakeElement('div'), im = fakeElement('img');
        t.elegooAttach(cell, im, 'http://127.0.0.1:5000/elegoo?ip=10.0.0.9', 'http://127.0.0.1:5000/elegoo/status?ip=10.0.0.9');
        const note = cell.children[0];
        check('the tile loads the relay address', im.src === 'http://127.0.0.1:5000/elegoo?ip=10.0.0.9');
        check('it says it is connecting', /Connecting/.test(note.textContent));
        im.fire('error');
        await tick(); await tick();
        check('the status is asked of the relay', sandbox.calls.fetched[0] === 'http://127.0.0.1:5000/elegoo/status?ip=10.0.0.9');
        check('a refused tile shows the busy message', note.textContent === 'Camera busy: too many viewers (Elegoo allows 4).');
        check('and offers Retry', note.children.length === 1 && note.children[0].textContent === 'Retry');
        note.children[0].onclick({ stopPropagation() {} });
        check('Retry loads the relay again with a new request', im.src === 'http://127.0.0.1:5000/elegoo?ip=10.0.0.9&n=1');
        check('Retry goes back to connecting', /Connecting/.test(note.textContent));
        im.fire('load');
        check('the first picture hides the note', note.style.display === 'none');
    }

    // ---- a status that cannot be fetched still gives words -----------------------------------------
    {
        const { t } = load({ statusFails: true });
        const cell = fakeElement('div'), im = fakeElement('img');
        t.elegooAttach(cell, im, 'u', 's');
        im.fire('error');
        await tick(); await tick();
        check('no status, still a sentence and Retry', cell.children[0].textContent.indexOf('camera') !== -1);
    }

    // ---- a parked tile (source removed on purpose) is not an error -------------------------------
    {
        const { sandbox, t } = load();
        const cell = fakeElement('div'), im = fakeElement('img');
        t.elegooAttach(cell, im, 'u', 's');
        im.removeAttribute('src');
        im.fire('error');
        await tick();
        check('no status request for a parked tile', sandbox.calls.fetched.length === 0);
    }

    // ---- printers from Printer settings --------------------------------------------------------
    {
        const { sandbox, t, w } = load({ known: false });
        w.__elegooSeed([{ ip: '10.0.0.9', name: 'CC1 shelf' }]);
        check('nothing is added before the hub list is read', sandbox.state.hosts.length === 0);
        check('applying it adds a tile', t.applyElegooSeed() === true);
        const h = sandbox.state.hosts[0];
        check('the tile is an Elegoo camera named after the printer', h.kind === 'elegoo' && h.ip === '10.0.0.9' && h.alias === 'CC1 shelf');
        check('and it is shown', sandbox.state.active.indexOf(h.id) !== -1);
        check('applying again adds nothing', t.applyElegooSeed() === false && sandbox.state.hosts.length === 1);
    }
    {
        const { sandbox, t, w } = load();
        sandbox.state.dismissed = ['10.0.0.9'];
        w.__elegooSeed([{ ip: '10.0.0.9', name: 'CC1 shelf' }, { ip: '10.0.0.10', name: 'CC1 desk' }]);
        check('a removed printer is not brought back, a new one is', sandbox.state.hosts.length === 1 && sandbox.state.hosts[0].ip === '10.0.0.10');
        check('a change saves and redraws', sandbox.calls.save === 1 && sandbox.calls.render === 1);
    }
    {
        const { sandbox, w } = load({ remote: true });
        w.__elegooSeed([{ ip: '10.0.0.9', name: 'x' }]);
        check('the phone page adds nothing', sandbox.state.hosts.length === 0);
    }
    {
        const { sandbox, t, w } = load();
        sandbox.state.hosts.push({ id: 'a', ip: 'http://10.0.0.9:80/', kind: 'elegoo' });
        w.__elegooSeed([{ ip: '10.0.0.9', name: 'CC1' }]);
        check('a tile typed by hand is not doubled', sandbox.state.hosts.length === 1);
    }

    console.log(passed + ' passed, ' + failures + ' failed');
    process.exit(failures ? 1 : 0);
})();

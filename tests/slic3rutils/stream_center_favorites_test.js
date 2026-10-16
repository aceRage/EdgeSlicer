// The hub page's filament lists with favourite filaments (resources/web/orca/stream_center.html,
// fillSelect / fillFilamentSelect). Plain Node, no packages, no browser: it slices the real
// functions out of the shipped page and runs them against a tiny DOM stand-in.
//
// /api/presets lists every filament once and marks the slicer's favourites with "favorite": the
// page shows them in a "Favorite filaments" optgroup on top (starred), then every filament under
// "All filaments"; the slot's filament is selected once. Without favourites the list is plain.
//
// Run: node tests/slic3rutils/stream_center_favorites_test.js
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'resources', 'web', 'orca', 'stream_center.html'), 'utf8');

function extractFunction(source, name) {
    const start = source.indexOf('function ' + name + '(');
    if (start < 0) throw new Error(name + ' not found - file structure changed');
    let i = source.indexOf('{', start), depth = 0;
    for (; i < source.length; i++) {
        if (source[i] === '{') depth++;
        else if (source[i] === '}' && --depth === 0) return source.slice(start, i + 1);
    }
    throw new Error('no end of ' + name);
}

// Just enough DOM for the two functions.
class Node {
    constructor(tag) { this.tagName = tag; this.children = []; this.textContent = ''; this.className = ''; }
    appendChild(c) { this.children.push(c); return c; }
    insertBefore(c, ref) { const i = this.children.indexOf(ref); this.children.splice(i < 0 ? 0 : i, 0, c); return c; }
    get firstChild() { return this.children[0] || null; }
    set innerHTML(v) { if (v === '') this.children = []; }
}
const document = { createElement: tag => new Node(tag) };
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }

const ctx = vm.createContext({ document, el });
vm.runInContext(extractFunction(html, 'fillSelect') + '\n' + extractFunction(html, 'fillFilamentSelect') +
                '\nthis.fillFilamentSelect = fillFilamentSelect;', ctx);

function options(sel) {
    const out = [];
    for (const c of sel.children) {
        if (c.tagName === 'optgroup') for (const o of c.children) out.push({ group: c.label, text: o.textContent, value: o.value, selected: !!o.selected });
        else out.push({ group: null, text: c.textContent, value: c.value, selected: !!c.selected });
    }
    return out;
}

const items = [
    { name: 'Bambu PLA Basic', value: 'Bambu PLA Basic @BBL X1C' },
    { name: 'Generic PETG', value: 'Generic PETG @BBL X1C', favorite: true },
    { name: 'Generic PLA', value: 'Generic PLA @BBL X1C', favorite: true },
];

// favourites on top, starred, then everything; the slot's filament selected once (the first copy)
{
    const sel = new Node('select');
    ctx.fillFilamentSelect(sel, items, 'Generic PLA @BBL X1C');
    const o = options(sel);
    assert.deepStrictEqual(sel.children.map(c => c.label), ['Favorite filaments', 'All filaments']);
    assert.deepStrictEqual(o.map(x => x.text), ['★ Generic PETG', '★ Generic PLA', 'Bambu PLA Basic', '★ Generic PETG', '★ Generic PLA']);
    assert.deepStrictEqual(o.filter(x => x.selected).map(x => [x.group, x.value]), [['Favorite filaments', 'Generic PLA @BBL X1C']]);
}
// a slot filament that is not a favourite is selected in "All filaments"
{
    const sel = new Node('select');
    ctx.fillFilamentSelect(sel, items, 'Bambu PLA Basic @BBL X1C');
    assert.deepStrictEqual(options(sel).filter(x => x.selected).map(x => [x.group, x.value]), [['All filaments', 'Bambu PLA Basic @BBL X1C']]);
}
// no favourites: the old plain list, no groups
{
    const plain = items.map(it => ({ name: it.name, value: it.value }));
    const sel = new Node('select');
    ctx.fillFilamentSelect(sel, plain, 'Generic PETG @BBL X1C');
    assert.ok(sel.children.every(c => c.tagName === 'option'));
    assert.deepStrictEqual(options(sel).map(x => x.text), ['Bambu PLA Basic', 'Generic PETG', 'Generic PLA']);
}
// a slot filament missing from the list is still shown (and selected), first
{
    const sel = new Node('select');
    ctx.fillFilamentSelect(sel, items, 'Hidden PLA');
    const o = options(sel);
    assert.deepStrictEqual([o[0].text, o[0].selected], ['Hidden PLA', true]);
}
console.log('stream_center_favorites_test: all checks passed');

// The Vendors page's "Max records per file" default (resources/web/home/csvmax.js).
// Plain node, no packages. Not wired into CMake/CI, like safehtml_model_test.js.
// Run: node tests/slic3rutils/vendor_csv_default_test.js
'use strict';
const assert = require('assert');
const path = require('path');
const { defaultMax, defaultRows, cleanRows, clean, PRESETS, CAP } = require(path.join(__dirname, '..', '..', 'resources', 'web', 'home', 'csvmax.js'));

const A = { id: 'a', limit: 200 };
const B = { id: 'b', limit: 0 };
const C = { id: 'c', limit: 500 };
const none = { last: null, byId: {} };

// nothing set anywhere: All
assert.strictEqual(defaultMax([B], none), 0);
assert.strictEqual(defaultMax([], none), 0);
assert.strictEqual(defaultMax([B], undefined), 0);
// one connector with its own limit
assert.strictEqual(defaultMax([A], none), 200);
// the user's own choice for that connector wins, including All
assert.strictEqual(defaultMax([A], { last: 1000, byId: { a: 100 } }), 100);
assert.strictEqual(defaultMax([A], { last: 1000, byId: { a: 0 } }), 0);
// the last choice applies to a connector without a limit and without a choice of its own
assert.strictEqual(defaultMax([B], { last: 100, byId: {} }), 100);
assert.strictEqual(defaultMax([B], { last: 100, byId: { b: 0 } }), 0);
// the connector's own limit beats the last choice
assert.strictEqual(defaultMax([A], { last: 100, byId: {} }), 200);
// several connectors share one set of files: the smallest limit wins
assert.strictEqual(defaultMax([A, C], none), 200);
assert.strictEqual(defaultMax([C, A, B], none), 200);
assert.strictEqual(defaultMax([B, C], none), 500);
assert.strictEqual(defaultMax([A, C], { last: 0, byId: { a: 300 } }), 300);
assert.strictEqual(defaultMax([A, C], { last: 0, byId: { a: 0 } }), 500); // All for A leaves C's limit
// several connectors, no limits: the last choice
assert.strictEqual(defaultMax([B, { id: 'd', limit: 0 }], { last: 1000, byId: {} }), 1000);
// bad stored values are ignored
assert.strictEqual(defaultMax([B], { last: 'x', byId: {} }), 0);
assert.strictEqual(defaultMax([B], { last: -5, byId: {} }), 0);
assert.strictEqual(defaultMax([{ id: 'e', limit: 'junk' }], none), 0);

// clean
assert.strictEqual(clean('250'), 250);
assert.strictEqual(clean(' 250 '), 250);
assert.strictEqual(clean(''), 0);
assert.strictEqual(clean('0'), 0);
assert.strictEqual(clean(2.9), 2);
assert.strictEqual(clean(null), 0);
assert.strictEqual(clean(CAP + 10), CAP);
assert.deepStrictEqual(PRESETS, [0, 100, 200, 500, 1000]);

// ---- rows: one row per model (the default) or per file ----
const R = (id, rows) => ({ id: id, rows: rows });
assert.strictEqual(defaultRows([R('a', '')], undefined), 'model');
assert.strictEqual(defaultRows([], { rowsLast: null, rowsById: {} }), 'model');
assert.strictEqual(defaultRows([R('a', 'file')], { rowsLast: null, rowsById: {} }), 'file');   // the connector's own setting
assert.strictEqual(defaultRows([R('a', 'file')], { rowsLast: 'model', rowsById: {} }), 'file'); // beats the last choice
assert.strictEqual(defaultRows([R('a', 'file')], { rowsLast: 'file', rowsById: { a: 'model' } }), 'model'); // the user's own choice wins
assert.strictEqual(defaultRows([R('a', '')], { rowsLast: 'file', rowsById: {} }), 'file');      // last choice
assert.strictEqual(defaultRows([R('a', '')], { rowsLast: 'bogus', rowsById: { a: 'x' } }), 'model');
// several connectors: agree -> that value; none set -> last; disagree -> last
assert.strictEqual(defaultRows([R('a', 'file'), R('b', 'file')], { rowsLast: 'model', rowsById: {} }), 'file');
assert.strictEqual(defaultRows([R('a', 'file'), R('b', '')], { rowsLast: 'model', rowsById: {} }), 'file');
assert.strictEqual(defaultRows([R('a', 'file'), R('b', 'model')], { rowsLast: 'file', rowsById: {} }), 'file');
assert.strictEqual(defaultRows([R('a', 'file'), R('b', 'model')], { rowsLast: null, rowsById: {} }), 'model');
assert.strictEqual(defaultRows([R('a', ''), R('b', '')], { rowsLast: 'file', rowsById: {} }), 'file');
assert.strictEqual(cleanRows('file'), 'file');
assert.strictEqual(cleanRows('Model'), '');
assert.strictEqual(cleanRows(null), '');

console.log('PASS vendor_csv_default_test');

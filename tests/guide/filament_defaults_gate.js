// Gate: the setup wizard's filament page (resources/web/guide/22) ticks the default filaments of the printers
// picked.
//
// When no filament is installed yet the page calls ChooseDefaultFilament(), which is to tick every filament row
// that holds one of the default materials of a picked printer model ("default_materials" of the machine model
// file, whole preset names such as "Generic PLA @U1 0.4 nozzle"). A row stands for the presets that share a short
// name (the part before the "@") and keeps their whole names in its "filalist" attribute.
//
// This runs the shipped 22.js in Node, with no browser: ChooseDefaultFilament() only reads the picked models and
// the rows and sets "checked", so $ is a stub that hands it those. The rows are built as SortUI() builds them. The
// models, their default materials and the filament names are read from the shipped resources/profiles.
//
// It checks:
//   * Snapmaker U1 and Bambu Lab A1: the rows of the model's default materials are ticked, and no other row
//   * a default material without an "@" still ticks the row whose whole name it is
//   * with no model picked nothing is ticked (and the function does not throw)
//   * rows ticked before are unticked first
//   * every model of every shipped vendor: each default material that names a filament of the vendor (or of the
//     filament library) has its row ticked
//
// Run: node tests/guide/filament_defaults_gate.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const RES = process.env.RES_DIR || path.join(__dirname, '..', '..', 'resources');
const PROFILES = path.join(RES, 'profiles');

let failures = 0, passed = 0;
function check(label, cond, extra) {
    if (cond) { passed++; console.log('ok   - ' + label); }
    else { failures++; console.error('FAIL - ' + label + (extra !== undefined ? '  (' + extra + ')' : '')); }
}

// ---- the page script, with $ standing for the two lists it reads --------------------------------------------
const page = { models: [], rows: [] };
const nothing = { hide() { return nothing; }, show() { return nothing; } };
function $(what) {
    if (what === '#MachineList input:gt(0)') return page.models;
    if (what === '#ItemBlockArea .MItem') return page.rows;
    if (typeof what === 'string') return nothing;
    // $(input).prop("checked", value)
    return { prop(name, value) { what[name] = value; return this; } };
}
const context = vm.createContext({ $, console });
vm.runInContext(fs.readFileSync(path.join(RES, 'web', 'guide', '22', '22.js'), 'utf8'), context);

function shortName(whole) { return whole.split('@')[0].trim(); }

// The rows as SortUI() makes them: one for each short name, its filalist the whole names with a ";" after each.
function makeRows(filamentNames, tickedBefore) {
    const byShort = new Map();
    for (const whole of filamentNames) {
        const key = shortName(whole);
        if (!byShort.has(key)) byShort.set(key, { name: key, filalist: '', checked: !!tickedBefore });
        byShort.get(key).filalist += whole.trim() + ';';
    }
    return Array.from(byShort.values()).map(input => ({
        input,
        getElementsByTagName() { return [{ getAttribute: a => input[a], set checked(v) { input.checked = v; }, get checked() { return input.checked; } }]; },
    }));
}

// Lays out the page for these models (picked: the ones ticked in the machine list) and runs the function.
// Returns the short names of the rows it left ticked, or what it threw.
function ticked(models, pickedModels, filamentNames, tickedBefore) {
    page.models = pickedModels.map(name => ({ getAttribute: () => name }));
    page.rows = makeRows(filamentNames, tickedBefore);
    context.m_ProfileItem = { model: models };
    try { vm.runInContext('ChooseDefaultFilament()', context); }
    catch (e) { return ['<threw ' + e.message + '>']; }
    return page.rows.filter(r => r.input.checked).map(r => r.input.name).sort();
}

// ---- the shipped profiles ------------------------------------------------------------------------------------
function readJson(file) { try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch (e) { return null; } }
function vendor(name) {
    const root = readJson(path.join(PROFILES, name + '.json'));
    if (!root) return null;
    const models = [];
    for (const m of root.machine_model_list || []) {
        const file = readJson(path.join(PROFILES, name, m.sub_path));
        if (file && typeof file.default_materials === 'string')
            models.push({ model: file.name || m.name, materials: file.default_materials });
    }
    return { models, filaments: (root.filament_list || []).map(f => f.name) };
}
const library = (vendor('OrcaFilamentLibrary') || { filaments: [] }).filaments;
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const wanted = (model, filaments) => Array.from(new Set(model.materials.split(';').filter(m => m !== '' && filaments.includes(m)).map(shortName))).sort();

// ============================================================ 1. two real printers
for (const [vendorName, modelName] of [['Snapmaker', 'Snapmaker U1'], ['BBL', 'Bambu Lab A1']]) {
    const v = vendor(vendorName);
    const model = v && v.models.find(m => m.model === modelName);
    check(modelName + ': the model file and its default materials are there', !!model && model.materials.includes('@'), model && model.materials);
    if (!model) continue;
    const filaments = v.filaments.concat(library);
    const want = wanted(model, filaments);
    const got = ticked(v.models, [modelName], filaments);
    check(modelName + ': it has default materials among the filaments (' + want.length + ' rows)', want.length > 0);
    check(modelName + ': the rows of its default materials are ticked, and no other', same(got, want), 'ticked: ' + JSON.stringify(got) + ' wanted: ' + JSON.stringify(want));
}

// ============================================================ 2. small made-up lists
{
    const models = [
        { model: 'Plain', materials: 'Generic PLA;Generic PETG @Plain 0.4 nozzle' },
        { model: 'Other', materials: 'Generic ABS @Other' },
    ];
    const filaments = ['Generic PLA', 'Generic PLA @Plain 0.6 nozzle', 'Generic PETG @Plain 0.4 nozzle', 'Generic ABS @Other', 'Generic TPU @Plain'];
    check('a default material without an "@" ticks the row whose whole name it is',
        same(ticked(models, ['Plain'], filaments), ['Generic PETG', 'Generic PLA']), JSON.stringify(ticked(models, ['Plain'], filaments)));
    check('with no model picked nothing is ticked, and nothing is thrown', same(ticked(models, [], filaments), []), JSON.stringify(ticked(models, [], filaments)));
    check('two picked models tick the rows of both', same(ticked(models, ['Plain', 'Other'], filaments), ['Generic ABS', 'Generic PETG', 'Generic PLA']));
    check('rows ticked before are unticked first', same(ticked(models, ['Other'], filaments, true), ['Generic ABS']));
    // A short name alone is not a default material: "Generic PLA" is asked for, and only "@"-names are in the row.
    check('a default material that names no filament in the row ticks nothing',
        same(ticked([{ model: 'Short', materials: 'Generic PLA' }], ['Short'], ['Generic PLA @Short']), []));
}

// ============================================================ 3. every shipped vendor
{
    let modelsSeen = 0, modelsRight = 0, rowsWanted = 0;
    const wrong = [];
    for (const file of fs.readdirSync(PROFILES).filter(f => f.endsWith('.json')).sort()) {
        const name = file.slice(0, -5);
        const v = vendor(name);
        if (!v) continue;
        const filaments = v.filaments.concat(library);
        for (const model of v.models) {
            const want = wanted(model, filaments);
            if (want.length === 0) continue;
            modelsSeen++; rowsWanted += want.length;
            if (same(ticked(v.models, [model.model], filaments), want)) modelsRight++;
            else if (wrong.length < 5) wrong.push(name + ' / ' + model.model);
        }
    }
    check('there are shipped models with default materials to check (' + modelsSeen + ' models, ' + rowsWanted + ' rows)', modelsSeen > 100);
    check('every one has the rows of its default materials ticked, and no other', modelsRight === modelsSeen, (modelsSeen - modelsRight) + ' wrong, e.g. ' + wrong.join('; '));
}

console.log(`\n${passed} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);

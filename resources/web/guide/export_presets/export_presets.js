// "Process presets (.zip)" and "Filament presets (.zip)" in File > Export > Export Preset Bundle:
// one row per user preset, with the same table (data_table.js) as Printer Selection. The page is
// loaded with ?kind=process or ?kind=filament. The slicer's dialog around it holds the export
// type radio buttons and OK / Cancel (CreatePresetsDialog.cpp).
//
// Slicer -> page: HandleStudio({command: 'response_export_rows', response: {rows, printers, vendors, materials, selected}})
//   rows[]    { id, name, printers[], all, lh (layer height mm | null), mtime (Unix s | null), vendor, material }
//   printers  every printer some row is listed under, sorted
//   selected  ids ticked so far (after a reload of the page), optional
// Page -> slicer (window.wx.postMessage):
//   export_table_ready                     the page is up; asks for the rows
//   export_table_selection {ids, count}    after every change of the ticks (ids = row ids)
//   export_table_cancel                    Esc with nothing open
//
// Ticks belong to the rows, so a preset stays ticked while a filter hides it; All / Clear all and
// the header checkbox act on the rows shown. The rows start sorted by printer, then preset name.
var PresetExport = (function () {
	'use strict';

	var T = DataTable.T;
	var KIND = (typeof GetQueryString === 'function' && GetQueryString('kind') === 'filament') ? 'filament' : 'process';

	function send(cmd, extra) {
		var m = { sequence_id: Math.round(new Date() / 1000), command: cmd };
		for (var k in (extra || {})) if (extra.hasOwnProperty(k)) m[k] = extra[k];
		SendWXMessage(JSON.stringify(m));
	}

	// 0.2 -> "0.20", 0.125 -> "0.125"
	function fmtLayer(v) {
		if (v === null) return '—';
		return v.toFixed(3).replace(/0$/, '');
	}
	function fmtDate(sec) {
		if (sec === null) return '—';
		var d = new Date(sec * 1000);
		try { return d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }); } catch (e) { }
		return d.toISOString().replace('T', ' ').slice(0, 16);
	}
	// What the Printer column shows for a row: its printer, or the first and how many more.
	function printerText(r) {
		if (r.all) return T('pe_all_printers', 'All printers');
		if (!r.printers.length) return '—';
		if (r.printers.length === 1) return r.printers[0];
		return T('pe_printer_more', '{first} +{n}', { first: r.printers[0], n: r.printers.length - 1 });
	}
	// The key the Printer column sorts by (and the rows start sorted by): all-printers presets
	// first, presets without a printer last.
	function printerSort(r) { return r.all ? '' : (r.printers[0] || null); }

	var COL_CHECK = { key: 'checked', tid: 'pe_export', label: 'Export', w: '112px', kind: 'bool', filter: 'set', cls: 'check',
		sortVal: function (r) { return r.checked ? 0 : 1; },
		setKeys: function (r) { return [r.checked ? 'yes' : 'no']; },
		setLabel: function (k) { return k === 'yes' ? T('pe_on', 'Selected') : T('pe_off', 'Not selected'); },
		setOrder: function (a, b) { return a === b ? 0 : (a === 'yes' ? -1 : 1); } };
	var COL_NAME = { key: 'name', tid: 'pe_name', label: 'Preset', w: 'minmax(200px, 3fr)', kind: 'text', filter: 'text',
		cell: function (r) { return { text: r.name, title: r.name }; } };
	var COL_PRINTER = { key: 'printers', tid: 'pe_printer', label: 'Printer', w: 'minmax(150px, 2fr)', kind: 'text', filter: 'set', listSearch: true,
		sortVal: printerSort,
		setKeys: function (r) { return r.printers; },
		cell: function (r) { return { text: printerText(r), title: r.all ? T('pe_all_printers_tip', 'Compatible with every printer') : r.printers.join('\n'), na: !r.printers.length }; } };
	var COL_MTIME = { key: 'mtime', tid: 'pe_modified', label: 'Last modified', w: '176px', kind: 'num',
		cell: function (r) { return { text: fmtDate(r.mtime), na: r.mtime === null }; } };
	function textCol(key, tid, label, w) {
		return { key: key, tid: tid, label: label, w: w, kind: 'text', filter: 'set', listSearch: true,
			setKeys: function (r) { return [r[key]]; },
			setLabel: function (k) { return k === '' ? '—' : k; },
			sortVal: function (r) { return r[key] === '' ? null : r[key]; },
			cell: function (r) { return { text: r[key] || '—', title: r[key], na: !r[key] }; } };
	}
	var COL_LAYER = { key: 'lh', tid: 'pe_layer', label: 'Layer height', w: '116px', kind: 'num', filter: 'range', cls: 'num', step: '0.01',
		tipTid: 'pe_layer_tip', tip: 'mm',
		fmt: fmtLayer,
		cell: function (r) { return { text: fmtLayer(r.lh), na: r.lh === null }; } };

	var cols, selects, tx;
	if (KIND === 'filament') {
		cols = [COL_CHECK, COL_NAME, COL_PRINTER, textCol('vendor', 'pe_vendor', 'Vendor', 'minmax(110px, 1.2fr)'),
			textCol('material', 'pe_material', 'Material', '110px'), COL_MTIME];
		selects = [
			{ key: 'printers', tid: 'pe_printer', label: 'Printer', allTid: 'pe_all_printers_opt', all: 'All printers', nTid: 'pe_n_printers', n: '{n} printers' },
			{ key: 'vendor', tid: 'pe_vendor', label: 'Vendor', allTid: 'pe_all_vendors', all: 'All vendors', nTid: 'pe_n_vendors', n: '{n} vendors' },
			{ key: 'material', tid: 'pe_material', label: 'Material', allTid: 'pe_all_materials', all: 'All materials', nTid: 'pe_n_materials', n: '{n} materials' }
		];
		tx = {
			search: ['pe_search_filament', 'Search preset, printer, vendor or material'],
			grid: ['pe_title_filament', 'Filament presets'],
			no_rows: ['pe_no_rows_filament', 'There are no user filament presets to export.'],
			none_match: ['pe_none_match_filament', 'No filament preset matches the search and filters.']
		};
	} else {
		cols = [COL_CHECK, COL_NAME, COL_PRINTER, COL_LAYER, COL_MTIME];
		selects = [{ key: 'printers', tid: 'pe_printer', label: 'Printer', allTid: 'pe_all_printers_opt', all: 'All printers', nTid: 'pe_n_printers', n: '{n} printers' }];
		tx = {
			search: ['pe_search', 'Search preset or printer'],
			grid: ['pe_title', 'Process presets'],
			no_rows: ['pe_no_rows', 'There are no user process presets to export.'],
			none_match: ['pe_none_match', 'No process preset matches the search and filters.']
		};
	}
	tx.enable_row = ['pe_select_row', 'Export {name}'];
	tx.on = ['pe_on', 'Selected'];
	tx.off = ['pe_off', 'Not selected'];
	tx.count_all = ['pe_count_all', '{n} presets · {on} selected'];
	tx.count = ['pe_count', '{shown} of {n} shown · {on} selected'];
	tx.scope_all = ['pe_scope_all', 'All / Clear all: every preset'];
	tx.scope = ['pe_scope', 'All / Clear all: the {shown} shown'];
	tx.scope_tip = ['pe_scope_tip', 'Applies to the {shown} presets shown by the search and filters ({on} of them selected)'];
	tx.range_hint = ['pe_range_hint', 'In mm. Presets without a layer height are hidden while a limit is set.'];
	tx.head_check = ['pe_head_check', 'Select or clear every preset shown'];

	var table = DataTable.create({
		headerCheck: true,
		storageKey: 'edge_export_cols_' + KIND,
		tx: tx,
		cols: cols,
		selects: selects,
		onSelectionChange: announce
	});

	function init(root) { table.init(root); }

	function load(resp) {
		var list = (resp && resp.rows) || [];
		var rows = [];
		for (var i = 0; i < list.length; i++) {
			var o = list[i];
			if (!o || typeof o.name !== 'string') continue;
			var printers = Array.isArray(o.printers) ? o.printers.filter(function (p) { return typeof p === 'string'; }) : [];
			var r = {
				id: typeof o.id === 'number' ? o.id : i, order: 0, name: o.name, printers: printers, all: !!o.all,
				lh: (typeof o.lh === 'number' && isFinite(o.lh) && o.lh > 0) ? o.lh : null,
				mtime: (typeof o.mtime === 'number' && o.mtime > 0) ? o.mtime : null,
				vendor: typeof o.vendor === 'string' ? o.vendor : '',
				material: typeof o.material === 'string' ? o.material : '',
				checked: false
			};
			r.hay = (r.name + '\u0000' + printers.join('\u0000') + '\u0000' + r.vendor + '\u0000' + r.material).toLowerCase();
			rows.push(r);
		}
		// "No sort" is printer, then preset name.
		rows.slice().sort(function (a, b) {
			var pa = printerSort(a), pb = printerSort(b);
			if (pa === null && pb !== null) return 1;
			if (pb === null && pa !== null) return -1;
			var c = pa === pb ? 0 : DataTable.cmpText(pa || '', pb || '');
			return c !== 0 ? c : DataTable.cmpText(a.name, b.name);
		}).forEach(function (r, n) { r.order = n; });
		// The engine keys its DOM by row id = array index. Ticks the slicer already knows (the page was
		// reloaded, e.g. after a theme change) come back.
		var ticked = {};
		((resp && resp.selected) || []).forEach(function (id) { ticked[id] = true; });
		rows.forEach(function (r, n) { r.sourceId = r.id; r.id = n; r.checked = !!ticked[r.sourceId]; });
		table.setRows(rows);
		announce();
	}

	// The ids the slicer gave the rows (not the table's own indexes) of the ticked rows.
	function selectedIds() {
		var out = [], rows = table.rows();
		for (var i = 0; i < rows.length; i++) if (rows[i].checked) out.push(rows[i].sourceId);
		return out;
	}
	function announce() {
		var sel = selectedIds();
		send('export_table_selection', { ids: sel, count: sel.length });
	}

	return {
		kind: KIND,
		init: init,
		load: load,
		selectedIds: selectedIds,
		closePopover: table.closePopover,
		_state: table._state
	};
})();

function OnInit()
{
	TranslatePage();
	PresetExport.init(document.getElementById('PtRoot'));
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "export_table_ready";
	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio(pVal)
{
	if (pVal['command'] == 'response_export_rows')
		PresetExport.load(pVal['response']);
}

document.addEventListener('keydown', function (e) {
	if (e.key === 'Escape') {
		// Esc first closes an open filter, then the slicer's dialog.
		if (PresetExport.closePopover())
			return;
		var tSend = {};
		tSend['sequence_id'] = Math.round(new Date() / 1000);
		tSend['command'] = "export_table_cancel";
		SendWXMessage(JSON.stringify(tSend));
	}
});

// Ctrl + wheel would zoom the page.
window.addEventListener('wheel', function (event) {
	if (event.ctrlKey === true || event.metaKey)
		event.preventDefault();
}, { passive: false });

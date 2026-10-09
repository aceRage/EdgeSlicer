// "Process presets (.zip)" in File > Export > Export Preset Bundle: one row per user process
// preset, with the same table (data_table.js) as Printer Selection. The slicer's dialog around it
// holds the export type radio buttons and OK / Cancel (CreatePresetsDialog.cpp).
//
// Slicer -> page: HandleStudio({command: 'response_process_presets', response: {rows, printers}})
//   rows[]    { id, name, printers[], all, inherits, lh (layer height mm | null), mtime (Unix s | null) }
//   printers  every printer some row is listed under, sorted
//   selected  ids ticked so far (after a reload of the page), optional
// Page -> slicer (window.wx.postMessage):
//   export_table_ready                     the page is up; asks for the rows
//   export_table_selection {ids, count}    after every change of the ticks (ids = row ids)
//   export_table_cancel                    Esc with nothing open
//
// Ticks belong to the rows, so a preset stays ticked while a filter hides it; All / Clear all and
// the header checkbox act on the rows shown.
var ProcessExport = (function () {
	'use strict';

	var T = DataTable.T, h = DataTable.h;

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

	var model = { printers: [] };
	var printerSelect = null;
	var table;

	// ---- the printer drop-down: "All printers" or one printer, kept in step with the Printer filter ----
	var MULTI = '__several__', multiOpt = null;
	function buildPrinterSelect(bar) {
		var wrap = h('label', 'pt-printer');
		wrap.appendChild(h('span', 'pt-printer-lbl', T('pe_printer', 'Printer')));
		printerSelect = h('select', 'pt-select');
		printerSelect.setAttribute('aria-label', T('pe_printer_filter', 'Show presets of printer'));
		printerSelect.addEventListener('change', function () {
			if (printerSelect.value === MULTI) return;
			var f = table.setFilter('printers');
			for (var k in f) if (f.hasOwnProperty(k)) delete f[k];
			if (printerSelect.value !== '')
				model.printers.forEach(function (p) { if (p !== printerSelect.value) f[p] = 1; });
			table.recompute(false);
		});
		wrap.appendChild(printerSelect);
		bar.appendChild(wrap);
	}
	function fillPrinterSelect() {
		printerSelect.textContent = '';
		multiOpt = null;
		var all = h('option', null, T('pe_all_printers_opt', 'All printers'));
		all.value = '';
		printerSelect.appendChild(all);
		model.printers.forEach(function (p) {
			var o = h('option', null, p);
			o.value = p;
			printerSelect.appendChild(o);
		});
	}
	// After the shown rows changed: show what the Printer filter (maybe set from the column's
	// funnel) amounts to.
	function syncPrinterSelect() {
		if (!printerSelect) return;
		var f = table.setFilter('printers');
		var inc = model.printers.filter(function (p) { return !f[p]; });
		if (multiOpt) { multiOpt.remove(); multiOpt = null; }
		if (inc.length === model.printers.length) printerSelect.value = '';
		else if (inc.length === 1) printerSelect.value = inc[0];
		else {
			multiOpt = h('option', null, T('pe_n_printers', '{n} printers', { n: inc.length }));
			multiOpt.value = MULTI;
			printerSelect.appendChild(multiOpt);
			printerSelect.value = MULTI;
		}
	}

	table = DataTable.create({
		headerCheck: true,
		tx: {
			search: ['pe_search', 'Search preset or printer'],
			grid: ['pe_title', 'Process presets'],
			none_match: ['pe_none_match', 'No process preset matches the search and filters.'],
			no_rows: ['pe_no_rows', 'There are no user process presets to export.'],
			enable_row: ['pe_select_row', 'Export {name}'],
			on: ['pe_on', 'Selected'],
			off: ['pe_off', 'Not selected'],
			count_all: ['pe_count_all', '{n} presets · {on} selected'],
			count: ['pe_count', '{shown} of {n} shown · {on} selected'],
			scope_all: ['pe_scope_all', 'All / Clear all: every preset'],
			scope: ['pe_scope', 'All / Clear all: the {shown} shown'],
			scope_tip: ['pe_scope_tip', 'Applies to the {shown} presets shown by the search and filters ({on} of them selected)'],
			range_hint: ['pe_range_hint', 'In mm. Presets without a layer height are hidden while a limit is set.'],
			head_check: ['pe_head_check', 'Select or clear every preset shown']
		},
		cols: [
			{ key: 'checked', tid: 'pe_export', label: 'Export', w: '112px', kind: 'bool', filter: 'set', cls: 'check',
				sortVal: function (r) { return r.checked ? 0 : 1; },
				setKeys: function (r) { return [r.checked ? 'yes' : 'no']; },
				setLabel: function (k) { return k === 'yes' ? T('pe_on', 'Selected') : T('pe_off', 'Not selected'); },
				setOrder: function (a, b) { return a === b ? 0 : (a === 'yes' ? -1 : 1); } },
			{ key: 'name', tid: 'pe_name', label: 'Preset', w: 'minmax(180px, 2.4fr)', kind: 'text', filter: 'text',
				cell: function (r) { return { text: r.name, title: r.name }; } },
			{ key: 'printers', tid: 'pe_printer', label: 'Printer', w: 'minmax(150px, 2fr)', kind: 'text', filter: 'set', listSearch: true,
				sortVal: function (r) { return r.all ? '' : (r.printers[0] || null); },
				setKeys: function (r) { return r.printers; },
				cell: function (r) { return { text: printerText(r), title: r.all ? T('pe_all_printers_tip', 'Compatible with every printer') : r.printers.join('\n'), na: !r.printers.length }; } },
			{ key: 'inherits', tid: 'pe_inherits', label: 'Inherits from', w: 'minmax(160px, 2fr)', kind: 'text', filter: 'set', listSearch: true,
				setKeys: function (r) { return [r.inherits]; },
				setLabel: function (k) { return k === '' ? '—' : k; },
				sortVal: function (r) { return r.inherits === '' ? null : r.inherits; },
				cell: function (r) { return { text: r.inherits || '—', title: r.inherits, na: !r.inherits }; } },
			{ key: 'lh', tid: 'pe_layer', label: 'Layer height', w: '116px', kind: 'num', filter: 'range', cls: 'num', step: '0.01',
				tipTid: 'pe_layer_tip', tip: 'mm',
				fmt: fmtLayer,
				cell: function (r) { return { text: fmtLayer(r.lh), na: r.lh === null }; } },
			{ key: 'mtime', tid: 'pe_modified', label: 'Last modified', w: '150px', kind: 'num',
				cell: function (r) { return { text: fmtDate(r.mtime), na: r.mtime === null }; } }
		],
		toolbar: buildPrinterSelect,
		onView: syncPrinterSelect,
		onSelectionChange: announce
	});

	function init(root) { table.init(root); }

	function load(resp) {
		var list = (resp && resp.rows) || [];
		var rows = [];
		model.printers = ((resp && resp.printers) || []).slice();
		for (var i = 0; i < list.length; i++) {
			var o = list[i];
			if (!o || typeof o.name !== 'string') continue;
			var printers = Array.isArray(o.printers) ? o.printers.filter(function (p) { return typeof p === 'string'; }) : [];
			var r = {
				id: typeof o.id === 'number' ? o.id : i, order: rows.length, name: o.name, printers: printers, all: !!o.all,
				inherits: typeof o.inherits === 'string' ? o.inherits : '',
				lh: (typeof o.lh === 'number' && isFinite(o.lh) && o.lh > 0) ? o.lh : null,
				mtime: (typeof o.mtime === 'number' && o.mtime > 0) ? o.mtime : null,
				checked: false
			};
			r.hay = (r.name + '\u0000' + printers.join('\u0000') + '\u0000' + r.inherits).toLowerCase();
			rows.push(r);
		}
		// The engine keys its DOM by row id = array index. Ticks the slicer already knows (the page was
		// reloaded, e.g. after a theme change) come back.
		var ticked = {};
		((resp && resp.selected) || []).forEach(function (id) { ticked[id] = true; });
		rows.forEach(function (r, n) { r.sourceId = r.id; r.id = n; r.checked = !!ticked[r.sourceId]; });
		fillPrinterSelect();
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
	ProcessExport.init(document.getElementById('PtRoot'));
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "export_table_ready";
	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio(pVal)
{
	if (pVal['command'] == 'response_process_presets')
		ProcessExport.load(pVal['response']);
}

document.addEventListener('keydown', function (e) {
	if (e.key === 'Escape') {
		// Esc first closes an open filter, then the slicer's dialog.
		if (ProcessExport.closePopover())
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

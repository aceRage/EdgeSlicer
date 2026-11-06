// The Filament manager (?kind=filament) and the Process manager (?kind=process): every filament or
// process preset in one table (data_table.js, as Printer Selection), with an Enabled box and a
// favourite star per row. The slicer side is src/slic3r/GUI/PresetManagerDialog.cpp; the rows are
// built by libslic3r/PresetManagerModel. Design: tests/design_preset_manager_tables.md.
//
// Slicer -> page: HandleStudio({command, ...})
//   pm_rows           {kind, rows[], printers[], current_printer}   (also re-sent; the page keeps its
//                     filters, sort, scroll and focused row: rows are identified by key)
//     row: {key, fav, name, members[], vendor, type, source: System|User, filament_id, version,
//           printers[], all, nozzles[], nozzle_t, nozzle_lo, nozzle_hi, bed_t, lh, tier, inherits,
//           mtime, enabled, lock: ''|'user'|'in_use', in_use, star, can_rename, can_delete}
//   pm_enabled_state  {rows: [{key, enabled, lock}]}   after an Enabled change was applied
//   pm_stars          {favs: [...]}                     the favourite keys (a star changed anywhere)
//   pm_escape         {}                                 Esc pressed outside the page
// Page -> slicer (window.wx.postMessage, JSON with command + sequence_id):
//   pm_ready                   asks for the rows
//   pm_enabled {changes: [{key, on}]}   the Enabled ticks, 250 ms after the last one (All / Clear all:
//                              one batch). Everything applies at once; there is no Confirm.
//   pm_star {key}              toggles a favourite (key = the row's fav)
//   pm_create, pm_edit {filament_id}    filaments: Create new / Edit printers... (close the dialog)
//   pm_close                   Close, or Esc with nothing open
//
// Keys on the focused row: Space = Enabled, F or * = star, arrows / Home / End / PageUp / PageDown
// move. F2 and Delete (Return and Backspace on macOS) say why they do nothing yet (phase 2: rename,
// delete). Ctrl+F: the search box. Esc: closes a filter popover, else cancels an edit (phase 2), else
// closes the dialog.
var PresetManager = (function () {
	'use strict';

	var T = DataTable.T, h = DataTable.h;
	var KIND = (typeof GetQueryString === 'function' && GetQueryString('kind') === 'process') ? 'process' : 'filament';
	var IS_MAC = /Mac OS X|Macintosh/.test(navigator.userAgent);
	var DEBOUNCE_MS = 250;
	var STAR = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 1.4l1.95 4.1 4.45.55-3.27 3.08.84 4.42L8 11.4l-3.97 2.15.84-4.42L1.6 6.05l4.45-.55z"/></svg>';

	var byKey = {};
	var currentPrinter = '';
	var firstLoad = true;
	var pending = {};          // key -> wanted Enabled state, not sent yet
	var timer = null;
	var statusTimer = null;
	var editing = null;        // phase 2: the row whose name is being edited

	function send(cmd, extra) {
		var m = { sequence_id: Math.round(new Date() / 1000), command: cmd };
		for (var k in (extra || {})) if (extra.hasOwnProperty(k)) m[k] = extra[k];
		SendWXMessage(JSON.stringify(m));
	}

	function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : null; }
	function strs(a) { return Array.isArray(a) ? a.filter(function (s) { return typeof s === 'string'; }) : []; }
	function fmtLayer(v) { return v === null ? '—' : v.toFixed(3).replace(/0$/, ''); }
	function fmtTemp(v) { return v === null ? '—' : String(Math.round(v)); }
	function fmtDate(sec) {
		if (sec === null) return '—';
		var d = new Date(sec * 1000);
		try { return d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }); } catch (e) { }
		return d.toISOString().replace('T', ' ').slice(0, 16);
	}
	function lockText(r) {
		if (r.lock === 'user') return T('pm_lock_user', 'User presets are always shown');
		if (r.lock === 'in_use') return T('pm_lock_in_use', 'In use in this project');
		return '';
	}
	function printerLabel(p) { return p === currentPrinter && p ? T('pm_current', '{name} (current)', { name: p }) : p; }
	function printerText(r) {
		if (r.all) return T('pm_any_printer', 'Any printer');
		if (!r.printers.length) return '—';
		if (r.printers.length === 1) return r.printers[0];
		return T('pm_printer_more', '{first} +{n}', { first: r.printers[0], n: r.printers.length - 1 });
	}
	function status(text) {
		var s = document.getElementById('PmStatus');
		if (!s) return;
		s.textContent = text || '';
		if (statusTimer) clearTimeout(statusTimer);
		if (text) statusTimer = setTimeout(function () { s.textContent = ''; }, 4000);
	}

	// ---- columns ----
	function yesNo(onTid, on, offTid, off) {
		return {
			setLabel: function (k) { return k === 'yes' ? T(onTid, on) : T(offTid, off); },
			setOrder: function (a, b) { return a === b ? 0 : (a === 'yes' ? -1 : 1); }
		};
	}
	var yn = yesNo('pm_on', 'Enabled', 'pm_off', 'Not enabled');
	var COL_CHECK = { key: 'checked', tid: 'pm_enabled', label: 'Enabled', w: '112px', kind: 'bool', filter: 'set', cls: 'check',
		sortVal: function (r) { return r.checked ? 0 : 1; },
		setKeys: function (r) { return [r.checked ? 'yes' : 'no']; },
		setLabel: yn.setLabel, setOrder: yn.setOrder };
	var ys = yesNo('pm_starred', 'Favourite', 'pm_unstarred', 'Not favourite');
	var COL_STAR = { key: 'star', tid: 'pm_star', label: 'Favourite', w: '96px', kind: 'bool', filter: 'set', cls: 'star',
		sortVal: function (r) { return r.star ? 0 : 1; },
		setKeys: function (r) { return [r.star ? 'yes' : 'no']; },
		setLabel: ys.setLabel, setOrder: ys.setOrder,
		node: function (r) {
			var b = h('button', 'pt-star' + (r.star ? ' on' : ''));
			b.type = 'button';
			b.dataset.act = 'star';
			b.tabIndex = -1;
			b.innerHTML = STAR;
			var t = r.star ? T('pm_unstar', 'Remove from favourites') : T('pm_star_tip', 'Add to favourites');
			b.title = t;
			b.setAttribute('aria-label', t + ': ' + r.name);
			b.setAttribute('aria-pressed', r.star ? 'true' : 'false');
			return b;
		} };
	var COL_NAME = { key: 'name', tid: 'pm_name', label: 'Name', w: 'minmax(180px, 3fr)', kind: 'text', filter: 'text',
		node: function (r) {
			var s = h('span', 'pm-name' + (r.in_use ? ' inuse' : ''), r.name);
			var t = r.name;
			if (r.members.length > 1 || (r.members.length === 1 && r.members[0] !== r.name)) t += '\n' + r.members.join('\n');
			if (r.in_use) t += '\n' + T('pm_in_use', 'In use');
			s.title = t;
			return s;
		} };
	function textCol(key, tid, label, w) {
		return { key: key, tid: tid, label: label, w: w, kind: 'text', filter: 'set', listSearch: true,
			setKeys: function (r) { return [r[key]]; },
			setLabel: function (k) { return k === '' ? '—' : k; },
			sortVal: function (r) { return r[key] === '' ? null : r[key]; },
			cell: function (r) { return { text: r[key] || '—', title: r[key], na: !r[key] }; } };
	}
	var COL_SOURCE = { key: 'source', tid: 'pm_source', label: 'Source', w: '90px', kind: 'text', filter: 'set',
		setKeys: function (r) { return [r.source]; },
		setLabel: function (k) { return k === 'User' ? T('pm_src_user', 'User') : T('pm_src_system', 'System'); },
		cell: function (r) { return { text: r.source === 'User' ? T('pm_src_user', 'User') : T('pm_src_system', 'System') }; } };
	var COL_PRINTERS = { key: 'printers', tid: KIND === 'process' ? 'pm_printer' : 'pm_printers', label: KIND === 'process' ? 'Printer' : 'Printers',
		w: 'minmax(150px, 2fr)', kind: 'text', filter: 'set', listSearch: true,
		sortVal: function (r) { return r.all ? '' : (r.printers[0] || null); },
		setKeys: function (r) { return r.printers; },
		setLabel: function (k) { return k === '' ? '—' : printerLabel(k); },
		setOrder: function (a, b) {
			if (a === b) return 0;
			if (a === currentPrinter) return -1;
			if (b === currentPrinter) return 1;
			if (a === '') return 1;
			if (b === '') return -1;
			return DataTable.cmpText(a, b);
		},
		cell: function (r) { return { text: printerText(r), title: r.printers.join('\n'), na: !r.printers.length && !r.all }; } };
	var COL_MTIME = { key: 'mtime', tid: 'pm_modified', label: 'Modified', w: '160px', kind: 'num',
		cell: function (r) {
			return { text: fmtDate(r.mtime), na: r.mtime === null,
				title: r.mtime === null && r.version ? T('pm_bundle_version', 'Vendor bundle {v}', { v: r.version }) : undefined };
		} };

	var cols, tx, selects;
	selects = [{ key: 'printers', tid: 'pm_printer', label: 'Printer', allTid: 'pm_all_printers', all: 'All printers', nTid: 'pm_n_printers', n: '{n} printers' }];
	if (KIND === 'filament') {
		cols = [COL_CHECK, COL_STAR, textCol('vendor', 'pm_vendor', 'Vendor', 'minmax(100px, 1.2fr)'), COL_NAME,
			textCol('type', 'pm_type', 'Type', '90px'), COL_SOURCE, COL_PRINTERS,
			{ key: 'nozzle_t', tid: 'pm_nozzle_t', label: 'Nozzle °C', w: '96px', kind: 'num', filter: 'range', cls: 'num', step: '5',
				tipTid: 'pm_nozzle_t_tip', tip: 'Nozzle temperature', fmt: fmtTemp,
				cell: function (r) { return { text: fmtTemp(r.nozzle_t), na: r.nozzle_t === null,
					title: (r.nozzle_lo !== null && r.nozzle_hi !== null) ? T('pm_range', 'Range {lo}–{hi} °C', { lo: fmtTemp(r.nozzle_lo), hi: fmtTemp(r.nozzle_hi) }) : undefined }; } },
			{ key: 'bed_t', tid: 'pm_bed_t', label: 'Bed °C', w: '84px', kind: 'num', filter: 'range', cls: 'num', step: '5',
				tipTid: 'pm_bed_t_tip', tip: 'Bed temperature for the current plate type', fmt: fmtTemp,
				cell: function (r) { return { text: fmtTemp(r.bed_t), na: r.bed_t === null }; } },
			COL_MTIME];
		tx = {
			search: ['pm_search_filament', 'Search name, vendor, type or printer'],
			grid: ['pm_title_filament', 'Filament manager'],
			no_rows: ['pm_no_rows', 'There are no presets.'],
			none_match: ['pm_none_match_filament', 'No filament matches the search and filters.'],
			count_all: ['pm_count_all_filament', '{n} filaments · {on} enabled'],
			count: ['pm_count_filament', '{shown} of {n} shown · {on} enabled'],
			scope_tip: ['pm_scope_tip_filament', 'Applies to the {shown} filaments shown by the search and filters ({on} of them enabled); locked rows are skipped']
		};
	} else {
		cols = [COL_CHECK, COL_STAR, COL_NAME, COL_PRINTERS,
			{ key: 'nozzles', tid: 'pm_nozzle', label: 'Nozzle', w: '96px', kind: 'text', filter: 'set',
				sortVal: function (r) { return r.nozzles.length ? +r.nozzles[0] : null; },
				setKeys: function (r) { return r.nozzles; },
				setLabel: function (k) { return k === '' ? '—' : k; },
				setOrder: function (a, b) { return a === '' ? 1 : b === '' ? -1 : (+a) - (+b); },
				cell: function (r) { return { text: r.nozzles.length ? r.nozzles.join(' / ') : '—', na: !r.nozzles.length }; } },
			{ key: 'lh', tid: 'pm_layer', label: 'Layer height', w: '124px', kind: 'num', filter: 'range', cls: 'num', step: '0.01',
				tipTid: 'pm_layer_tip', tip: 'mm', fmt: fmtLayer,
				cell: function (r) { return { text: fmtLayer(r.lh), na: r.lh === null }; } },
			textCol('tier', 'pm_tier', 'Quality / parent', 'minmax(140px, 1.2fr)'),
			COL_SOURCE, COL_MTIME];
		tx = {
			search: ['pm_search_process', 'Search name, printer or quality'],
			grid: ['pm_title_process', 'Process manager'],
			no_rows: ['pm_no_rows', 'There are no presets.'],
			none_match: ['pm_none_match_process', 'No process preset matches the search and filters.'],
			count_all: ['pm_count_all_process', '{n} process presets · {on} enabled'],
			count: ['pm_count_process', '{shown} of {n} shown · {on} enabled'],
			scope_tip: ['pm_scope_tip_process', 'Applies to the {shown} presets shown by the search and filters ({on} of them enabled); locked rows are skipped']
		};
	}
	tx.enable_row = ['pm_enable_row', 'Enable {name}'];
	tx.on = ['pm_on', 'Enabled'];
	tx.off = ['pm_off', 'Not enabled'];
	tx.scope_all = ['pm_scope_all', 'All / Clear all: every row'];
	tx.scope = ['pm_scope', 'All / Clear all: the {shown} shown'];
	tx.range_hint = ['pm_range_hint', 'Rows without this value are hidden while a limit is set.'];
	tx.head_check = ['pm_head_check', 'Enable or disable every row shown'];

	var editBtn = null;
	var table = DataTable.create({
		headerCheck: true,
		storageKey: 'edge_pm_cols_' + KIND,
		tx: tx,
		cols: cols,
		selects: selects,
		rowClick: 'focus',
		rowKey: function (r) { return r.key; },
		locked: function (r) { return r.lock ? (lockText(r) || true) : false; },
		onSelectionChange: onTicks,
		onAction: function (act, r) { if (act === 'star') star(r); },
		onRowKey: onRowKey,
		onFocusRow: updateButtons,
		toolbar: function (bar) {
			if (KIND !== 'filament') return;
			var create = h('div', 'SmallBtn', T('pm_create', 'Create new'));
			create.id = 'PmCreate';
			create.title = T('pm_create_tip', 'Create a new filament preset (closes this dialog)');
			editBtn = h('div', 'SmallBtn', T('pm_edit', 'Edit printers…'));
			editBtn.id = 'PmEdit';
			[create, editBtn].forEach(function (b) {
				b.setAttribute('role', 'button');
				b.tabIndex = 0;
				var act = function () { if (b === create) send('pm_create'); else editFocused(); };
				b.addEventListener('click', act);
				b.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); act(); } });
				bar.appendChild(b);
			});
			updateButtons(null);
		}
	});

	function editable(r) { return !!(r && r.source === 'User' && r.filament_id); }
	function updateButtons() {
		if (!editBtn) return;
		var r = table.focusedRow();
		var ok = editable(r);
		editBtn.classList.toggle('disabled', !ok);
		editBtn.setAttribute('aria-disabled', ok ? 'false' : 'true');
		editBtn.style.opacity = ok ? '' : '0.5';
		editBtn.title = ok ? T('pm_edit_tip', 'Edit the printers of this filament (closes this dialog)') : T('pm_edit_none', 'Select one of your own filaments first');
	}
	function editFocused() {
		var r = table.focusedRow();
		if (!editable(r)) { status(T('pm_edit_none', 'Select one of your own filaments first')); return; }
		flush();
		send('pm_edit', { filament_id: r.filament_id });
	}

	// ---- Enabled: applied at once, in batches ----
	function onTicks() {
		var rows = table.rows();
		for (var i = 0; i < rows.length; i++) {
			var r = rows[i];
			if (r.checked !== r.enabled) pending[r.key] = r.checked;
			else delete pending[r.key];
		}
		if (timer) clearTimeout(timer);
		timer = setTimeout(flush, DEBOUNCE_MS);
	}
	function flush() {
		if (timer) { clearTimeout(timer); timer = null; }
		var changes = [];
		for (var k in pending) if (pending.hasOwnProperty(k)) {
			changes.push({ key: k, on: pending[k] });
			if (byKey[k]) byKey[k].enabled = pending[k];
		}
		pending = {};
		if (changes.length) send('pm_enabled', { changes: changes });
		return changes.length;
	}
	function enabledState(list) {
		(list || []).forEach(function (s) {
			var r = byKey[s && s.key];
			if (!r) return;
			r.enabled = !!s.enabled;
			r.lock = typeof s.lock === 'string' ? s.lock : '';
			if (!pending.hasOwnProperty(r.key)) r.checked = r.enabled;
			table.repaintRow(r);
		});
		table.notifySelection();
	}

	// ---- stars ----
	function star(r) {
		if (!r.fav) return;
		var on = !r.star;
		table.rows().forEach(function (x) { if (x.fav === r.fav) { x.star = on; table.repaintRow(x); } });
		send('pm_star', { key: r.fav });
	}
	function stars(favs) {
		var set = {};
		(favs || []).forEach(function (k) { set[k] = true; });
		table.rows().forEach(function (x) { x.star = !!set[x.fav]; });
		table.repaint();
	}

	// ---- keys ----
	function onRowKey(e, r) {
		var t = e.target && e.target.tagName;
		if ((t === 'BUTTON' || t === 'INPUT') && (e.key === ' ' || e.key === 'Enter')) return false;
		if (e.ctrlKey || e.metaKey || e.altKey) return false;
		if (e.key === ' ' || e.key === 'Spacebar') {
			if (!table.toggleRow(r)) status(lockText(r));
			return true;
		}
		if (e.key === 'f' || e.key === 'F' || e.key === '*') { star(r); return true; }
		if (e.key === 'F2' || (IS_MAC && e.key === 'Enter')) {
			status(r.source === 'System' ? T('pm_no_rename_system', 'System presets cannot be renamed') : T('pm_no_rename_yet', 'Renaming presets here comes in a later version'));
			return true;
		}
		if (e.key === 'Delete' || (IS_MAC && e.key === 'Backspace')) {
			status(r.source === 'System' ? T('pm_no_delete_system', 'System presets cannot be deleted') : T('pm_no_delete_yet', 'Deleting presets here comes in a later version'));
			return true;
		}
		return false;
	}

	// Esc: a filter popover first, then an edit (phase 2), then the dialog. Returns what it did.
	function escape() {
		if (table.closePopover()) return 'popover';
		if (editing) { editing = null; return 'edit'; }
		close();
		return 'close';
	}
	function close() {
		flush();
		send('pm_close');
	}

	// ---- rows ----
	function load(resp) {
		var list = (resp && resp.rows) || [];
		currentPrinter = (resp && typeof resp.current_printer === 'string') ? resp.current_printer : '';
		var rows = [];
		byKey = {};
		for (var i = 0; i < list.length; i++) {
			var o = list[i];
			if (!o || typeof o.key !== 'string' || typeof o.name !== 'string' || byKey[o.key]) continue;
			var r = {
				id: rows.length, order: rows.length, key: o.key, fav: typeof o.fav === 'string' ? o.fav : '',
				name: o.name, members: strs(o.members), vendor: typeof o.vendor === 'string' ? o.vendor : '',
				type: typeof o.type === 'string' ? o.type : '', source: o.source === 'User' ? 'User' : 'System',
				filament_id: typeof o.filament_id === 'string' ? o.filament_id : '', version: typeof o.version === 'string' ? o.version : '',
				printers: strs(o.printers), all: !!o.all, nozzles: strs(o.nozzles),
				nozzle_t: num(o.nozzle_t), nozzle_lo: num(o.nozzle_lo), nozzle_hi: num(o.nozzle_hi), bed_t: num(o.bed_t),
				lh: (num(o.lh) !== null && o.lh > 0) ? o.lh : null, tier: typeof o.tier === 'string' ? o.tier : '',
				inherits: typeof o.inherits === 'string' ? o.inherits : '',
				mtime: (num(o.mtime) !== null && o.mtime > 0) ? o.mtime : null,
				enabled: !!o.enabled, lock: typeof o.lock === 'string' ? o.lock : '', in_use: !!o.in_use, star: !!o.star,
				can_rename: !!o.can_rename, can_delete: !!o.can_delete
			};
			r.checked = pending.hasOwnProperty(r.key) ? pending[r.key] : r.enabled;
			r.hay = [r.name, r.vendor, r.type, r.tier, r.printers.join('\u0000'), r.members.join('\u0000')].join('\u0000').toLowerCase();
			rows.push(r);
			byKey[r.key] = r;
		}
		table.setRows(rows, { keepScroll: !firstLoad });
		if (firstLoad) {
			firstLoad = false;
			// The Process manager starts on the current printer (thousands of rows otherwise); another
			// printer, or all of them, is one pick away in the Printer drop-down.
			if (KIND === 'process' && currentPrinter && rows.some(function (x) { return x.printers.indexOf(currentPrinter) >= 0; })) {
				var f = table.setFilter('printers');
				var seen = {};
				rows.forEach(function (x) {
					(x.printers.length ? x.printers : ['']).forEach(function (p) { if (p !== currentPrinter) seen[p] = 1; });
				});
				for (var p in seen) if (seen.hasOwnProperty(p)) f[p] = 1;
				table.recompute(false);
			}
		}
		updateButtons();
	}

	function init(root) {
		table.init(root);
		var note = document.getElementById('PmNote');
		if (note) note.textContent = T('pm_note', 'Changes are saved at once.');
		var closeBtn = document.getElementById('CloseBtn');
		if (closeBtn) {
			closeBtn.textContent = T('pm_close', 'Close');
			closeBtn.addEventListener('click', close);
			closeBtn.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); close(); } });
		}
	}

	return {
		kind: KIND,
		init: init,
		load: load,
		enabledState: enabledState,
		stars: stars,
		escape: escape,
		flush: flush,
		table: table,
		pending: function () { return pending; },
		_state: table._state
	};
})();

function OnInit()
{
	PresetManager.init(document.getElementById('PtRoot'));
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = 'pm_ready';
	tSend['kind'] = PresetManager.kind;
	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio(pVal)
{
	var cmd = pVal && pVal['command'];
	if (cmd === 'pm_rows') PresetManager.load(pVal);
	else if (cmd === 'pm_enabled_state') PresetManager.enabledState(pVal['rows']);
	else if (cmd === 'pm_stars') PresetManager.stars(pVal['favs']);
	else if (cmd === 'pm_escape') PresetManager.escape();
}

document.addEventListener('keydown', function (e) {
	if (e.key === 'Escape') {
		e.preventDefault();
		PresetManager.escape();
		return;
	}
	if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
		e.preventDefault();
		var s = PresetManager.table.search();
		if (s) { s.focus(); s.select(); }
	}
});

// Ctrl + wheel would zoom the page.
window.addEventListener('wheel', function (event) {
	if (event.ctrlKey === true || event.metaKey)
		event.preventDefault();
}, { passive: false });

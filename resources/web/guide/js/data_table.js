// A sortable, filterable, virtualised table with a select checkbox per row: the engine behind
// Printer Selection (printer_table.js: guide/24 and the wizard's guide/21) and the Process presets
// list of Export Preset Bundle (guide/export_process). Styles: ../css/printer_table.css (pt-*).
//
//   var table = DataTable.create(cfg);  table.init(rootElement);  table.setRows(rows);
//
// A row is a plain object with, at least:
//   id       its index in the array given to setRows (the engine uses it as the DOM key)
//   order    the original order (a stable sort key; "no sort" shows rows by it)
//   checked  the select state
//   hay      lower-case text the search box looks in
// plus whatever the columns read. cfg:
//   cols[]   { key, tid, label, w, kind: 'bool'|'text'|'num'|'none', cls, tipTid, tip,
//              filter: 'set'|'text'|'range'|undefined, listSearch,
//              sortVal(r)    value to sort by (default r[key]; null / undefined sort last)
//              setKeys(r)    "set" filters: the keys of a row (default [String(r[key])]); a row is
//                            hidden only when all of its keys are unticked
//              setLabel(k)   "set" filters: the text of a key (default the key)
//              rangeVal(r)   "range" filters: the number (default r[key], null = unknown)
//              textVal(r)    "text" filters: the text (default r[key])
//              cell(r)       { text, title, na } for a plain cell (default r[key])
//              node(r)       an element for a custom cell (the printer icon) }
//   checkedKey   the column whose cells hold the select checkbox (default 'checked')
//   nameKey      the column the sort falls back to for ties (default 'name')
//   headerCheck  true: the checkbox cell's header gets a select-all-shown checkbox (the row
//                checkboxes then sit left, under it)
//   tx           texts as {name: [tid, English]}, see TEXTS below (plus no_rows: shown when the
//                table has no rows at all)
//   group(r)     the rows that are ticked / unticked together with r (default [r])
//   onSelectionChange()   after the user changes a tick
//   onView()              after the shown rows changed (search, filter, sort)
//   toolbar(bar, api)     lets the page add controls after the search box
//   selects[]    drop-downs after the search box, each one a "set" filter of a column:
//                { key, tid, label, allTid, all: 'All printers', nTid, n: '{n} printers' }
//                (one value, or "All ..."; "{n} ..." when the column's funnel picked several)
//   storageKey   remember dragged column widths in localStorage under this key
// Every column but the last can be resized by dragging the right edge of its header (double-click
// an edge: back to the default widths). Until a column is dragged the layout is cfg.cols[].w.
// Search matches every word of the box anywhere in r.hay. All / Clear all (and the header
// checkbox) act on the rows shown.
var DataTable = (function () {
	'use strict';

	var OVERSCAN = 8;

	// ---- text: the page language when LangText has the key, English otherwise ----
	function lang() {
		var l = null;
		try { l = (typeof GetQueryString === 'function') ? GetQueryString('lang') : null; } catch (e) { }
		if (!l) { try { l = localStorage.getItem('BambuWebLang'); } catch (e) { } }
		return l;
	}
	function T(key, fallback, args) {
		var s = fallback;
		try {
			var L = (typeof LangText !== 'undefined') ? LangText : null;
			var l = lang();
			if (L && l && L[l] && L[l][key]) s = L[l][key];
			else if (L && L.en && L.en[key]) s = L.en[key];
		} catch (e) { }
		if (args) s = s.replace(/\{(\w+)\}/g, function (m, k) { return args.hasOwnProperty(k) ? args[k] : m; });
		return s;
	}

	// The texts the table shows, each [text id, English] (Printer Selection's); a page overrides
	// some in cfg.tx. {name} placeholders are filled by the engine.
	var TEXTS = {
		search: ['pt_search', 'Search brand or model'],
		grid: ['t10', 'Printer Selection'],
		none_match: ['pt_none_match', 'No printer matches the search and filters.'],
		enable_row: ['pt_enable_row', 'Enable {name}'],
		on: ['pt_on', 'Enabled'],
		off: ['pt_off', 'Not enabled'],
		all: ['t11', 'All'],
		clear_all: ['t12', 'Clear all'],
		count_all: ['pt_count_all', '{n} printers · {on} enabled'],
		count: ['pt_count', '{shown} of {n} shown · {on} enabled'],
		scope_all: ['pt_scope_all', 'All / Clear all: every printer'],
		scope: ['pt_scope', 'All / Clear all: the {shown} shown'],
		scope_tip: ['pt_scope_tip', 'Applies to the {shown} printers shown by the search and filters ({on} of them enabled)'],
		range_hint: ['pt_range_hint', 'In mm. Printers without this value are hidden while a limit is set.'],
		head_check: ['pt_head_check', 'Select all the rows shown']
	};

	var collator = (typeof Intl !== 'undefined' && Intl.Collator) ? new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' }) : null;
	function cmpText(a, b) { return collator ? collator.compare(a, b) : (a < b ? -1 : a > b ? 1 : 0); }

	function tokens(s) { return (s || '').toLowerCase().match(/\S+/g) || []; }
	function isEmpty(o) { for (var k in o) if (o.hasOwnProperty(k)) return false; return true; }
	function h(tag, cls, text) {
		var n = document.createElement(tag);
		if (cls) n.className = cls;
		if (text !== undefined && text !== null) n.textContent = text;
		return n;
	}
	var FUNNEL = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1 2h14l-5.5 6.5V14l-3 -1.5V8.5z"/></svg>';
	var SEARCH = '<svg id="search-icon" width="16px" height="16px" aria-hidden="true"><path d="M6.5,2A4.505,4.505,0,0,0,2,6.5a.5.5,0,0,0,1,0A3.5,3.5,0,0,1,6.5,3a.5.5,0,0,0,0-1Z"/><path d="M14.854,14.146l-3.423-3.422a6.518,6.518,0,1,0-.707.707l3.422,3.423a.5.5,0,0,0,.708-.708ZM1,6.5A5.5,5.5,0,1,1,6.5,12,5.507,5.507,0,0,1,1,6.5Z"/></svg>';

	function create(cfg) {
		var COLS = cfg.cols;
		var CHECKED = cfg.checkedKey || 'checked';
		var NAME = cfg.nameKey || 'name';

		// text lookup: cfg.tx[name] overrides TEXTS[name]
		function X(name, args) {
			var t = (cfg.tx && cfg.tx[name]) || TEXTS[name];
			return T(t[0], t[1], args);
		}

		function col(key) { for (var i = 0; i < COLS.length; i++) if (COLS[i].key === key) return COLS[i]; return null; }

		// ---- state ----
		var rows = [];          // every row
		var view = [];          // rows shown, in display order
		var state = { search: '', sort: { key: null, dir: 1 }, f: {} };
		var el = {};            // DOM refs
		var rendered = {};      // row id -> element currently in the body
		var pop = null;         // open filter popover { node, col }
		var rowH = 36;
		var rafPending = false;
		var api = null;
		var userW = null;       // px widths once a column was dragged (the last one stays flexible)
		var selects = [];       // { cfg, node, multi } of the toolbar drop-downs

		function resetFilters() {
			state.f = {};
			COLS.forEach(function (c) {
				if (c.filter === 'set') state.f[c.key] = {};
				else if (c.filter === 'text') state.f[c.key] = '';
				else if (c.filter === 'range') state.f[c.key] = { min: null, max: null };
			});
		}
		resetFilters();

		// ---- values ----
		function setKeysOf(c, r) {
			if (c.setKeys) { var k = c.setKeys(r); return k.length ? k : ['']; }
			return [String(r[c.key])];
		}
		function setLabelOf(c, k) { return c.setLabel ? c.setLabel(k) : k; }
		function sortValOf(c, r) { return c.sortVal ? c.sortVal(r) : r[c.key]; }
		function rangeValOf(c, r) { return c.rangeVal ? c.rangeVal(r) : r[c.key]; }
		function textValOf(c, r) { return String(c.textVal ? c.textVal(r) : r[c.key]); }
		function colName(c) { return T(c.tid, c.label); }
		function fmt(c, v) { return c.fmt ? c.fmt(v) : String(v); }

		// A row passes a "set" filter when at least one of its keys is still ticked.
		function passesSet(c, r) {
			var f = state.f[c.key], ks = setKeysOf(c, r);
			for (var i = 0; i < ks.length; i++) if (!f[ks[i]]) return true;
			return false;
		}
		function passes(r, except) {
			if (state.search) {
				var hay = r.hay, t = tokens(state.search);
				for (var i = 0; i < t.length; i++) if (hay.indexOf(t[i]) < 0) return false;
			}
			for (var s = 0; s < COLS.length; s++) {
				var c = COLS[s];
				if (!c.filter || c.key === except) continue;
				var f = state.f[c.key];
				if (c.filter === 'set') {
					if (!isEmpty(f) && !passesSet(c, r)) return false;
				} else if (c.filter === 'text') {
					if (f) {
						var n = textValOf(c, r).toLowerCase(), nt = tokens(f);
						for (var j = 0; j < nt.length; j++) if (n.indexOf(nt[j]) < 0) return false;
					}
				} else if (c.filter === 'range') {
					if (f.min === null && f.max === null) continue;
					var v = rangeValOf(c, r);
					if (v === null || v === undefined) return false;
					if (f.min !== null && v < f.min) return false;
					if (f.max !== null && v > f.max) return false;
				}
			}
			return true;
		}
		function filterActive(c) {
			if (!c.filter) return false;
			var f = state.f[c.key];
			if (c.filter === 'set') return !isEmpty(f);
			if (c.filter === 'text') return !!f;
			return f.min !== null || f.max !== null;
		}
		function anyFilter() {
			if (state.search) return true;
			for (var i = 0; i < COLS.length; i++) if (filterActive(COLS[i])) return true;
			return false;
		}

		function compare(a, b) {
			var k = state.sort.key;
			if (!k) return a.order - b.order;
			var c = col(k);
			var va = sortValOf(c, a), vb = sortValOf(c, b);
			var na = (va === null || va === undefined), nb = (vb === null || vb === undefined);
			if (na && nb) return a.order - b.order;
			if (na) return 1;      // unknown values last in either direction
			if (nb) return -1;
			var d = (typeof va === 'string') ? cmpText(va, vb) : va - vb;
			if (d !== 0) return d * state.sort.dir;
			if (k !== NAME) { d = cmpText(String(a[NAME]), String(b[NAME])); if (d !== 0) return d; }
			return a.order - b.order;
		}

		function recompute(keepScroll) {
			var out = [];
			for (var i = 0; i < rows.length; i++) if (passes(rows[i], null)) out.push(rows[i]);
			out.sort(compare);
			view = out;
			el.spacer.style.height = (view.length * rowH) + 'px';
			if (!keepScroll) el.body.scrollTop = 0;
			var noRows = !rows.length && cfg.tx && cfg.tx.no_rows;
			el.empty.textContent = noRows ? X('no_rows') : X('none_match');
			el.empty.style.display = ((rows.length || noRows) && !view.length) ? 'block' : 'none';
			renderRows(true);
			renderHead();
			renderChips();
			renderCounts();
			syncSelects();
			if (cfg.onView) cfg.onView();
		}

		// ---- skeleton ----
		function init(root) {
			el.root = root;
			root.textContent = '';

			var bar = h('div', 'pt-toolbar');
			var sw = h('div', 'search');
			el.search = h('input', 'searchTerm');
			el.search.type = 'text';
			el.search.placeholder = X('search');
			el.search.setAttribute('aria-label', X('search'));
			el.search.addEventListener('input', function () { state.search = el.search.value; recompute(false); });
			sw.appendChild(el.search);
			sw.insertAdjacentHTML('beforeend', SEARCH);
			bar.appendChild(sw);
			el.bar = bar;
			(cfg.selects || []).forEach(function (s) { buildSelect(bar, s); });
			if (cfg.toolbar) cfg.toolbar(bar, api);
			el.count = h('span', 'pt-count');
			el.count.setAttribute('role', 'status');
			el.count.setAttribute('aria-live', 'polite');
			bar.appendChild(el.count);
			bar.appendChild(h('div', 'pt-spacer'));
			el.scope = h('span', 'pt-scope');
			bar.appendChild(el.scope);
			el.all = h('div', 'SmallBtn_Green', X('all'));
			el.none = h('div', 'SmallBtn', X('clear_all'));
			[el.all, el.none].forEach(function (b, i) {
				b.setAttribute('role', 'button');
				b.tabIndex = 0;
				var act = function () { setShown(i === 0); };
				b.addEventListener('click', act);
				b.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); act(); } });
			});
			bar.appendChild(el.all);
			bar.appendChild(el.none);
			root.appendChild(bar);

			el.chips = h('div', 'pt-chips');
			root.appendChild(el.chips);

			var grid = h('div', 'pt-grid' + (cfg.headerCheck ? ' hc' : ''));
			grid.setAttribute('role', 'grid');
			grid.setAttribute('aria-label', X('grid'));
			el.grid = grid;
			el.head = h('div', 'pt-row pt-head');
			el.head.setAttribute('role', 'row');
			COLS.forEach(function (c) {
				var th = h('div', 'pt-th' + (c.cls === 'num' ? ' num' : ''));
				th.setAttribute('role', 'columnheader');
				th.dataset.key = c.key;
				if (c.key === CHECKED && cfg.headerCheck) {
					var hc = h('input', 'pt-hcheck');
					hc.type = 'checkbox';
					hc.title = X('head_check');
					hc.setAttribute('aria-label', X('head_check'));
					hc.addEventListener('change', function () { setShown(hc.checked); });
					th.appendChild(hc);
					el.hcheck = hc;
				}
				if (c.kind !== 'none') {
					var b = h('button', 'pt-sort');
					b.type = 'button';
					var lbl = h('span', 'lbl', colName(c));
					var arrow = h('span', 'arrow');
					arrow.setAttribute('aria-hidden', 'true');
					if (c.cls === 'num') { b.appendChild(arrow); b.appendChild(lbl); } else { b.appendChild(lbl); b.appendChild(arrow); }
					b.title = (c.tip ? T(c.tipTid, c.tip) + '. ' : '') + T('pt_sort_tip', 'Click to sort');
					b.addEventListener('click', function () { cycleSort(c.key); });
					th.appendChild(b);
					c.arrow = arrow;
					if (c.filter) {
						var fb = h('button', 'pt-filter-btn');
						fb.type = 'button';
						fb.innerHTML = FUNNEL;
						fb.title = T('pt_filter_tip', 'Filter {col}', { col: colName(c) });
						fb.setAttribute('aria-label', fb.title);
						fb.setAttribute('aria-haspopup', 'dialog');
						fb.addEventListener('click', function (e) { e.stopPropagation(); togglePopover(c, th); });
						th.appendChild(fb);
						c.fbtn = fb;
					}
				}
				if (COLS.indexOf(c) < COLS.length - 1) {
					var rz = h('div', 'pt-resize');
					rz.title = T('pt_resize_tip', 'Drag to resize the column; double-click to reset all widths');
					rz.addEventListener('mousedown', function (e) { startResize(e, COLS.indexOf(c)); });
					rz.addEventListener('click', function (e) { e.stopPropagation(); });
					rz.addEventListener('dblclick', function (e) { e.stopPropagation(); userW = null; applyTemplate(); saveWidths(); schedule(); });
					th.appendChild(rz);
				}
				c.th = th;
				el.head.appendChild(th);
			});
			loadWidths();
			applyTemplate();
			grid.appendChild(el.head);

			el.body = h('div', 'pt-body ZScrol');
			el.body.setAttribute('role', 'rowgroup');
			el.spacer = h('div', 'pt-spacer-y');
			el.body.appendChild(el.spacer);
			el.body.addEventListener('scroll', schedule);
			el.body.addEventListener('click', onBodyClick);
			el.body.addEventListener('change', onBodyChange);
			el.body.addEventListener('keydown', onBodyKey);
			el.body.addEventListener('mouseover', onBodyOver);
			grid.appendChild(el.body);
			el.empty = h('div', 'pt-empty', X('none_match'));
			el.empty.style.display = 'none';
			grid.appendChild(el.empty);
			root.appendChild(grid);

			window.addEventListener('resize', function () { closePopover(); schedule(); });
			document.addEventListener('mousedown', function (e) {
				if (pop && !pop.node.contains(e.target) && !(pop.col.fbtn && pop.col.fbtn.contains(e.target))) closePopover();
			}, true);
			recompute(false);
		}

		// ---- data ----
		function setRows(list) {
			rows = list;
			fillSelects();
			clearRendered();
			recompute(false);
		}

		function groupOf(r) { return cfg.group ? cfg.group(r) : [r]; }
		function setChecked(r, on) {
			var same = groupOf(r);
			for (var i = 0; i < same.length; i++) {
				same[i].checked = on;
				var node = rendered[same[i].id];
				if (node) paintChecked(node, same[i]);
			}
		}
		function changed() {
			renderCounts();
			renderChips();
			if (cfg.onSelectionChange) cfg.onSelectionChange();
		}
		// All / Clear all (and the header checkbox): the rows shown only. The view is not
		// re-filtered here, so a "selected" filter keeps the rows on screen until the filters change.
		function setShown(on) {
			for (var i = 0; i < view.length; i++) setChecked(view[i], on);
			changed();
		}

		// ---- body ----
		function clearRendered() {
			for (var id in rendered) if (rendered.hasOwnProperty(id)) rendered[id].remove();
			rendered = {};
		}
		function schedule() {
			if (rafPending) return;
			rafPending = true;
			(window.requestAnimationFrame || setTimeout)(function () { rafPending = false; renderRows(false); });
		}
		function paintChecked(node, r) {
			var cb = node.firstChild.firstChild;
			cb.checked = r.checked;
			node.classList.toggle('on', r.checked);
			node.setAttribute('aria-selected', r.checked ? 'true' : 'false');
		}
		function makeRow(r) {
			var row = h('div', 'pt-row');
			row.setAttribute('role', 'row');
			row.dataset.id = r.id;
			for (var i = 0; i < COLS.length; i++) {
				var c = COLS[i];
				var td;
				if (c.key === CHECKED) {
					td = h('div', 'pt-td check');
					var cb = h('input');
					cb.type = 'checkbox';
					cb.setAttribute('aria-label', X('enable_row', { name: String(r[NAME]) }));
					td.appendChild(cb);
				} else if (c.node) {
					td = h('div', 'pt-td' + (c.cls ? ' ' + c.cls : ''));
					td.appendChild(c.node(r));
				} else {
					var cell = c.cell ? c.cell(r) : { text: String(r[c.key]) };
					td = h('div', 'pt-td' + (c.cls === 'num' ? ' num' : '') + (cell.na ? ' na' : ''), cell.text);
					// A title richer than the text (a printer list) always shows; otherwise the full text
					// shows only when the cell cuts it off (onBodyOver).
					if (cell.title && cell.title !== cell.text) td.title = cell.title;
					else td.dataset.full = cell.text;
				}
				td.setAttribute('role', 'gridcell');
				row.appendChild(td);
			}
			paintChecked(row, r);
			return row;
		}
		function renderRows(all) {
			if (!el.body) return;
			var top = el.body.scrollTop, hgt = el.body.clientHeight || 400;
			var first = Math.max(0, Math.floor(top / rowH) - OVERSCAN);
			var last = Math.min(view.length - 1, Math.ceil((top + hgt) / rowH) + OVERSCAN);
			var want = {};
			for (var p = first; p <= last; p++) want[view[p].id] = p;
			for (var id in rendered) {
				if (rendered.hasOwnProperty(id) && !want.hasOwnProperty(id)) { rendered[id].remove(); delete rendered[id]; }
			}
			for (var q = first; q <= last; q++) {
				var r = view[q];
				var node = rendered[r.id];
				if (!node) { node = makeRow(r); rendered[r.id] = node; el.body.appendChild(node); }
				else if (all) paintChecked(node, r);
				node.style.transform = 'translateY(' + (q * rowH) + 'px)';
				node.dataset.pos = q;
				node.setAttribute('aria-rowindex', q + 2);
			}
			// Keep the header columns over the body columns when the body shows a scrollbar.
			el.head.style.paddingRight = Math.max(0, el.body.offsetWidth - el.body.clientWidth) + 'px';
		}
		function rowOf(target) {
			var n = target;
			while (n && n !== el.body) { if (n.classList && n.classList.contains('pt-row')) return n; n = n.parentNode; }
			return null;
		}
		function onBodyClick(e) {
			if (e.target.tagName === 'INPUT') return; // the checkbox's own change event handles it
			var node = rowOf(e.target);
			if (!node) return;
			var r = rows[+node.dataset.id];
			setChecked(r, !r.checked);
			changed();
		}
		function onBodyChange(e) {
			var node = rowOf(e.target);
			if (!node) return;
			setChecked(rows[+node.dataset.id], e.target.checked);
			changed();
		}
		function onBodyOver(e) {
			var td = e.target;
			if (!td || !td.dataset || td.dataset.full === undefined) return;
			if (td.scrollWidth > td.clientWidth + 1) td.title = td.dataset.full;
			else td.removeAttribute('title');
		}
		function onBodyKey(e) {
			if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp' && e.key !== 'Home' && e.key !== 'End') return;
			var node = rowOf(e.target);
			if (!node) return;
			e.preventDefault();
			var pos = +node.dataset.pos;
			pos = e.key === 'ArrowDown' ? pos + 1 : e.key === 'ArrowUp' ? pos - 1 : e.key === 'Home' ? 0 : view.length - 1;
			focusPos(Math.max(0, Math.min(view.length - 1, pos)));
		}
		function focusPos(pos) {
			var b = el.body, y = pos * rowH;
			if (y < b.scrollTop) b.scrollTop = y;
			else if (y + rowH > b.scrollTop + b.clientHeight) b.scrollTop = y + rowH - b.clientHeight;
			renderRows(false);
			var node = rendered[view[pos].id];
			if (node) node.firstChild.firstChild.focus();
		}

		// ---- column widths ----
		function minW(i) { return COLS[i].minW || (COLS[i].key === CHECKED ? 64 : 48); }
		function colTemplate() {
			if (!userW) return COLS.map(function (c) { return c.w; }).join(' ');
			var last = COLS.length - 1;
			return userW.map(function (w, i) { return i === last ? 'minmax(' + minW(i) + 'px, 1fr)' : Math.round(w) + 'px'; }).join(' ');
		}
		function applyTemplate() { if (el.grid) el.grid.style.setProperty('--pt-cols', colTemplate()); }
		function loadWidths() {
			if (!cfg.storageKey) return;
			try {
				var w = JSON.parse(localStorage.getItem(cfg.storageKey) || 'null');
				if (w && w.length === COLS.length && w.every(function (x) { return typeof x === 'number' && x > 0; })) userW = w;
			} catch (e) { }
		}
		function saveWidths() {
			if (!cfg.storageKey) return;
			try { if (userW) localStorage.setItem(cfg.storageKey, JSON.stringify(userW)); else localStorage.removeItem(cfg.storageKey); } catch (e) { }
		}
		// Dragging the right edge of column i: it grows at the expense of the columns to its right
		// (nearest first, down to their minimum), or shrinks and gives the space to its neighbour.
		// The total stays; the last column is flexible, so it follows the window.
		function startResize(e, i) {
			e.preventDefault();
			e.stopPropagation();
			closePopover();
			var w0 = COLS.map(function (c) { return c.th.getBoundingClientRect().width; });
			var x0 = e.clientX;
			document.body.classList.add('pt-resizing');
			function move(ev) {
				var dx = ev.clientX - x0, w = w0.slice();
				if (dx >= 0) {
					var need = dx, taken = 0;
					for (var j = i + 1; j < COLS.length && need > 0; j++) {
						var t = Math.min(Math.max(0, w[j] - minW(j)), need);
						w[j] -= t; need -= t; taken += t;
					}
					w[i] = w0[i] + taken;
				} else {
					var give = Math.min(-dx, Math.max(0, w0[i] - minW(i)));
					w[i] = w0[i] - give;
					w[i + 1] += give;
				}
				userW = w;
				applyTemplate();
			}
			function up() {
				document.removeEventListener('mousemove', move, true);
				document.removeEventListener('mouseup', up, true);
				document.body.classList.remove('pt-resizing');
				saveWidths();
				schedule();
			}
			document.addEventListener('mousemove', move, true);
			document.addEventListener('mouseup', up, true);
		}

		// ---- toolbar drop-downs: a "set" filter of one column as a single choice ----
		var MULTI = '__several__';
		function buildSelect(bar, s) {
			var c = col(s.key);
			var wrap = h('label', 'pt-printer');
			wrap.appendChild(h('span', 'pt-printer-lbl', T(s.tid, s.label)));
			var sel = h('select', 'pt-select');
			sel.setAttribute('aria-label', T(s.tid, s.label));
			sel.dataset.key = s.key;
			var entry = { cfg: s, col: c, node: sel, multi: null };
			sel.addEventListener('change', function () {
				if (sel.value === MULTI) return;
				var f = state.f[s.key];
				for (var k in f) if (f.hasOwnProperty(k)) delete f[k];
				if (sel.value !== '') universe(c).forEach(function (k) { if (k !== sel.value) f[k] = 1; });
				recompute(false);
			});
			wrap.appendChild(sel);
			bar.appendChild(wrap);
			selects.push(entry);
		}
		function fillSelects() {
			selects.forEach(function (e) {
				e.node.textContent = '';
				e.multi = null;
				var all = h('option', null, T(e.cfg.allTid, e.cfg.all));
				all.value = '';
				e.node.appendChild(all);
				sortKeys(e.col, universe(e.col)).forEach(function (k) {
					var o = h('option', null, setLabelOf(e.col, k));
					o.value = k;
					e.node.appendChild(o);
				});
			});
		}
		// What the column's filter (maybe set from its funnel or a chip) amounts to.
		function syncSelects() {
			selects.forEach(function (e) {
				var f = state.f[e.cfg.key], all = universe(e.col);
				var inc = all.filter(function (k) { return !f[k]; });
				if (e.multi) { e.multi.remove(); e.multi = null; }
				if (inc.length === all.length) e.node.value = '';
				else if (inc.length === 1) e.node.value = inc[0];
				else {
					e.multi = h('option', null, T(e.cfg.nTid, e.cfg.n, { n: inc.length }));
					e.multi.value = MULTI;
					e.node.appendChild(e.multi);
					e.node.value = MULTI;
				}
			});
		}

		// ---- header, chips, counts ----
		function cycleSort(key) {
			var s = state.sort;
			if (s.key !== key) { s.key = key; s.dir = 1; }
			else if (s.dir === 1) s.dir = -1;
			else { s.key = null; s.dir = 1; }
			recompute(false);
		}
		function renderHead() {
			COLS.forEach(function (c) {
				if (!c.th || c.kind === 'none') return;
				var on = state.sort.key === c.key;
				c.arrow.textContent = on ? (state.sort.dir === 1 ? '▲' : '▼') : '';
				c.th.setAttribute('aria-sort', on ? (state.sort.dir === 1 ? 'ascending' : 'descending') : 'none');
				if (c.fbtn) c.fbtn.classList.toggle('on', filterActive(c));
			});
		}
		function universe(c) {
			var seen = {}, out = [];
			for (var i = 0; i < rows.length; i++) {
				var ks = setKeysOf(c, rows[i]);
				for (var j = 0; j < ks.length; j++) {
					if (!seen.hasOwnProperty(ks[j])) { seen[ks[j]] = 1; out.push(ks[j]); }
				}
			}
			return out;
		}
		function sortKeys(c, keys) {
			keys.sort(function (a, b) {
				if (c.setOrder) return c.setOrder(a, b);
				if (c.kind === 'num') return a === '' ? 1 : b === '' ? -1 : (+a) - (+b);
				return cmpText(setLabelOf(c, a), setLabelOf(c, b));
			});
			return keys;
		}
		function chipText(c) {
			var f = state.f[c.key], name = colName(c);
			if (c.filter === 'set') {
				var all = universe(c);
				var inc = sortKeys(c, all.filter(function (k) { return !f[k]; }));
				var shown = inc.length <= 3 ? inc.map(function (k) { return setLabelOf(c, k); }).join(', ')
					: T('pt_n_of_m', '{n} of {m}', { n: inc.length, m: all.length });
				return name + ': ' + (inc.length ? shown : T('pt_nothing', 'none'));
			}
			if (c.filter === 'text') return name + ': “' + f + '”';
			if (f.min !== null && f.max !== null) return name + ': ' + fmt(c, f.min) + '–' + fmt(c, f.max);
			if (f.min !== null) return name + ' ≥ ' + fmt(c, f.min);
			return name + ' ≤ ' + fmt(c, f.max);
		}
		function clearFilter(c) {
			if (c.filter === 'set') state.f[c.key] = {};
			else if (c.filter === 'text') state.f[c.key] = '';
			else if (c.filter === 'range') state.f[c.key] = { min: null, max: null };
		}
		function renderChips() {
			el.chips.textContent = '';
			COLS.forEach(function (c) {
				if (!filterActive(c)) return;
				var chip = h('span', 'pt-chip');
				var t = chipText(c);
				var s = h('span', null, t);
				s.title = t;
				chip.appendChild(s);
				var x = h('button', null, '✕');
				x.type = 'button';
				x.title = T('pt_remove_filter', 'Remove this filter');
				x.setAttribute('aria-label', x.title + ': ' + t);
				x.addEventListener('click', function () { clearFilter(c); closePopover(); recompute(false); });
				chip.appendChild(x);
				el.chips.appendChild(chip);
			});
			if (anyFilter()) {
				var clr = h('button', 'pt-link', T('pt_clear_filters', 'Clear search and filters'));
				clr.type = 'button';
				clr.addEventListener('click', function () { clearAll(); });
				el.chips.appendChild(clr);
			}
		}
		function clearAll() {
			COLS.forEach(clearFilter);
			state.search = '';
			el.search.value = '';
			closePopover();
			recompute(false);
		}
		function renderCounts() {
			var on = 0;
			for (var i = 0; i < rows.length; i++) if (rows[i].checked) on++;
			var shownOn = 0;
			for (var j = 0; j < view.length; j++) if (view[j].checked) shownOn++;
			el.count.textContent = view.length === rows.length
				? X('count_all', { n: rows.length, on: on })
				: X('count', { shown: view.length, n: rows.length, on: on });
			el.scope.textContent = view.length === rows.length ? X('scope_all') : X('scope', { shown: view.length });
			var tip = X('scope_tip', { shown: view.length, on: shownOn });
			el.all.title = tip;
			el.none.title = tip;
			if (el.hcheck) {
				el.hcheck.checked = view.length > 0 && shownOn === view.length;
				el.hcheck.indeterminate = shownOn > 0 && shownOn < view.length;
				el.hcheck.disabled = view.length === 0;
			}
		}

		// ---- filter popover ----
		function closePopover() {
			if (!pop) return false;
			var c = pop.col;
			pop.node.remove();
			pop = null;
			if (c.fbtn) c.fbtn.setAttribute('aria-expanded', 'false');
			return true;
		}
		function togglePopover(c, th) {
			var was = pop && pop.col === c;
			closePopover();
			if (!was) openPopover(c, th);
		}
		function openPopover(c, th) {
			var node = h('div', 'pt-pop');
			node.setAttribute('role', 'dialog');
			node.setAttribute('aria-label', T('pt_filter_tip', 'Filter {col}', { col: colName(c) }));
			node.appendChild(h('div', 'pt-pop-title', T('pt_filter_tip', 'Filter {col}', { col: colName(c) })));
			var focusEl = null;
			if (c.filter === 'set') focusEl = buildSetFilter(c, node);
			else if (c.filter === 'text') focusEl = buildTextFilter(c, node);
			else focusEl = buildRangeFilter(c, node);
			node.addEventListener('keydown', function (e) {
				if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closePopover(); if (c.fbtn) c.fbtn.focus(); }
			});
			document.body.appendChild(node);
			pop = { node: node, col: c };
			if (c.fbtn) c.fbtn.setAttribute('aria-expanded', 'true');
			var rc = th.getBoundingClientRect(), w = node.offsetWidth;
			var left = Math.max(8, Math.min(rc.left, window.innerWidth - w - 8));
			node.style.left = left + 'px';
			node.style.top = (rc.bottom + 2) + 'px';
			node.style.maxHeight = Math.max(160, window.innerHeight - rc.bottom - 12) + 'px';
			node.style.overflowY = 'auto';
			if (focusEl) focusEl.focus();
		}
		function buildSetFilter(c, node) {
			var f = state.f[c.key];
			var counts = {};
			for (var i = 0; i < rows.length; i++) {
				if (!passes(rows[i], c.key)) continue;
				var ks = setKeysOf(c, rows[i]);
				for (var j = 0; j < ks.length; j++) counts[ks[j]] = (counts[ks[j]] || 0) + 1;
			}
			var keys = sortKeys(c, universe(c).filter(function (k) { return counts[k] || f[k]; }));
			var q = null;
			if (c.listSearch) {
				q = h('input');
				q.type = 'text';
				q.placeholder = T('pt_find', 'Find…');
				q.setAttribute('aria-label', T('pt_find', 'Find…'));
				node.appendChild(q);
			}
			var list = h('div', 'pt-opts');
			var boxes = [];
			keys.forEach(function (k) {
				var lab = h('label', 'pt-opt');
				var cb = h('input');
				cb.type = 'checkbox';
				cb.checked = !f[k];
				cb.addEventListener('change', function () {
					if (cb.checked) delete f[k]; else f[k] = 1;
					recompute(false);
				});
				lab.appendChild(cb);
				lab.appendChild(h('span', null, setLabelOf(c, k)));
				lab.appendChild(h('span', 'n', String(counts[k] || 0)));
				lab.dataset.text = setLabelOf(c, k).toLowerCase();
				list.appendChild(lab);
				boxes.push({ k: k, cb: cb, lab: lab });
			});
			node.appendChild(list);
			if (q) q.addEventListener('input', function () {
				var t = q.value.toLowerCase();
				boxes.forEach(function (b) { b.lab.style.display = b.lab.dataset.text.indexOf(t) >= 0 ? '' : 'none'; });
			});
			var btns = h('div', 'pt-pop-btns');
			var visible = function () { return boxes.filter(function (b) { return b.lab.style.display !== 'none'; }); };
			var selAll = h('button', 'pt-link', T('pt_select_all', 'Select all'));
			selAll.type = 'button';
			selAll.addEventListener('click', function () { visible().forEach(function (b) { b.cb.checked = true; delete f[b.k]; }); recompute(false); });
			var selNone = h('button', 'pt-link', T('pt_select_none', 'Select none'));
			selNone.type = 'button';
			selNone.addEventListener('click', function () { visible().forEach(function (b) { b.cb.checked = false; f[b.k] = 1; }); recompute(false); });
			btns.appendChild(selAll);
			btns.appendChild(selNone);
			node.appendChild(btns);
			return q || (boxes.length ? boxes[0].cb : null);
		}
		function buildTextFilter(c, node) {
			var inp = h('input');
			inp.type = 'text';
			inp.value = state.f[c.key];
			inp.placeholder = T('pt_contains', 'Contains…');
			inp.setAttribute('aria-label', T('pt_contains', 'Contains…'));
			inp.addEventListener('input', function () { state.f[c.key] = inp.value.trim(); recompute(false); });
			inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') closePopover(); });
			node.appendChild(inp);
			return inp;
		}
		function buildRangeFilter(c, node) {
			var lim = state.f[c.key];
			var box = h('div', 'pt-range');
			var mk = function (which, label) {
				var id = 'pt-' + c.key + '-' + which;
				var l = h('label', null, label);
				l.htmlFor = id;
				var inp = h('input');
				inp.type = 'number';
				inp.id = id;
				inp.min = '0';
				inp.step = c.step || '1';
				inp.value = lim[which] === null ? '' : String(lim[which]);
				inp.placeholder = T('pt_any', 'any');
				inp.addEventListener('input', function () {
					var v = inp.value.trim() === '' ? null : parseFloat(inp.value);
					lim[which] = (v === null || isNaN(v)) ? null : v;
					recompute(false);
				});
				inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') closePopover(); });
				box.appendChild(l);
				box.appendChild(inp);
				return inp;
			};
			var first = mk('min', T('pt_min', 'At least'));
			mk('max', T('pt_max', 'At most'));
			node.appendChild(box);
			node.appendChild(h('div', 'pt-hint', X('range_hint')));
			return first;
		}

		api = {
			init: init,
			setRows: setRows,
			rows: function () { return rows; },
			view: function () { return view; },
			recompute: recompute,
			clearAll: clearAll,
			closePopover: closePopover,
			// Page controls that set a "set" filter from outside (the export page's printer list).
			setFilter: function (key) { return state.f[key]; },
			repaint: function () { clearRendered(); recompute(true); },
			notifySelection: changed,
			_state: function () { return { rows: rows, view: view, state: state }; }
		};
		return api;
	}

	return { create: create, T: T, h: h, cmpText: cmpText };
})();

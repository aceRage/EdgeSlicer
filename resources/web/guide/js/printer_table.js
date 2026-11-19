// Printer Selection as one table, shared by guide/24 (sidebar "Select/Remove printers") and
// guide/21 (the setup wizard's printer page).
//
// One row per printer model (all its nozzle variants together, as before): Enabled, icon, Brand,
// Model, X/Y/Z (mm) and Toolheads. Every column sorts (click the title: up, down, back to the
// original order) and filters (the funnel: a value list for Enabled, Brand and Toolheads, text for
// Model, min/max for the sizes), plus one search box over brand and model. All / Clear all act
// on the rows currently shown.
//
// The rows come from the slicer's "response_userguide_profile" (WebGuideDialog.cpp): per model
// vendor, model, name, cover, nozzle_diameter ("0.4;0.2"), nozzle_selected (non-empty = enabled)
// and size_x / size_y / size_z / extruders (null when unknown, shown as a dash).
//
// The body is virtualised (only the rows in view exist in the DOM), so ~1000 models stay light
// and the cover images load only for rows that scroll into view.
//
// The table engine (sorting, filters, chips, counts, virtual body) is data_table.js; this file is
// Printer Selection's columns and rows.
var PrinterTable = (function () {
	'use strict';

	var VENDOR_PRIORITY = ['Snapmaker'];
	var BRAND_NAMES = { BBL: 'Bambu Lab', Custom: 'Custom Printer', Other: 'Orca colosseum' };
	var T = DataTable.T;

	// ---- values ----
	function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : null; }
	function fmt(v) {
		if (v === null) return '—';
		var r = Math.round(v);
		return Math.abs(v - r) < 0.1 ? String(r) : v.toFixed(1);
	}
	function numCell(v) { return { text: fmt(v), na: v === null }; }
	function sizeCol(key, tid, label, tipTid, tip) {
		return { key: key, tid: tid, label: label, w: '84px', kind: 'num', filter: 'range', cls: 'num', tipTid: tipTid, tip: tip,
			fmt: fmt, cell: function (r) { return numCell(r[key]); } };
	}

	var byKey = {};         // vendor + "\n" + model -> [rows] (a duplicate entry toggles along)

	var table = DataTable.create({
		cols: [
			{ key: 'checked', tid: 'pt_enabled', label: 'Enabled', w: '88px', kind: 'bool', filter: 'set', cls: 'check',
				sortVal: function (r) { return r.checked ? 0 : 1; },
				setKeys: function (r) { return [r.checked ? 'yes' : 'no']; },
				setLabel: function (k) { return k === 'yes' ? T('pt_on', 'Enabled') : T('pt_off', 'Not enabled'); },
				setOrder: function (a, b) { return a === b ? 0 : (a === 'yes' ? -1 : 1); } },
			{ key: 'icon', label: '', w: '40px', kind: 'none', cls: 'icon',
				node: function (r) {
					if (r.cover) {
						var img = document.createElement('img');
						img.alt = '';
						img.decoding = 'async';
						img.draggable = false;
						img.onerror = function () { this.onerror = null; var d = document.createElement('div'); d.className = 'noimg'; this.replaceWith(d); };
						img.src = r.cover;
						return img;
					}
					var d = document.createElement('div');
					d.className = 'noimg';
					return d;
				} },
			{ key: 'brand', tid: 'pt_brand', label: 'Brand', w: 'minmax(96px, 1fr)', kind: 'text', filter: 'set', listSearch: true,
				cell: function (r) { return { text: r.brand, title: r.brand }; } },
			{ key: 'name', tid: 'pt_model', label: 'Model', w: 'minmax(150px, 2.2fr)', kind: 'text', filter: 'text',
				cell: function (r) { return { text: r.name, title: r.name }; } },
			sizeCol('x', 'pt_x', 'X mm', 'pt_x_tip', 'Bed width, from the printer\'s default (0.4 mm nozzle) preset'),
			sizeCol('y', 'pt_y', 'Y mm', 'pt_y_tip', 'Bed depth, from the printer\'s default (0.4 mm nozzle) preset'),
			sizeCol('z', 'pt_z', 'Z mm', 'pt_z_tip', 'Printable height, from the printer\'s default (0.4 mm nozzle) preset'),
			{ key: 'th', tid: 'pt_toolheads', label: 'Toolheads', w: '104px', kind: 'num', filter: 'set', cls: 'num',
				tipTid: 'pt_toolheads_tip', tip: 'Number of extruders / toolheads',
				setKeys: function (r) { return [r.th === null ? '' : String(r.th)]; },
				setLabel: function (k) { return k === '' ? '—' : k; },
				cell: function (r) { return { text: r.th === null ? '—' : String(r.th), na: r.th === null }; } }
		],
		group: function (r) { return byKey[r.vendor + '\n' + r.model] || [r]; }
	});

	function init(root) { table.init(root); }

	// ---- data ----
	function load(resp) {
		var list = (resp && resp.model) || [];
		var pri = [], rest = [];
		for (var i = 0; i < list.length; i++) {
			var m = list[i];
			if (!m || typeof m.model !== 'string' || typeof m.vendor !== 'string') continue;
			(VENDOR_PRIORITY.indexOf(m.vendor) >= 0 ? pri : rest).push(m);
		}
		pri.sort(function (a, b) { return VENDOR_PRIORITY.indexOf(a.vendor) - VENDOR_PRIORITY.indexOf(b.vendor); });
		var all = pri.concat(rest);
		var rows = [];
		byKey = {};
		for (var n = 0; n < all.length; n++) {
			var o = all[n];
			var brand = BRAND_NAMES.hasOwnProperty(o.vendor) ? BRAND_NAMES[o.vendor] : o.vendor;
			var name = (typeof o.name === 'string' && o.name) ? o.name : o.model;
			var th = num(o.extruders);
			var r = {
				id: n, order: n, vendor: o.vendor, model: o.model, name: name, brand: brand,
				cover: typeof o.cover === 'string' ? o.cover : '',
				nozzles: typeof o.nozzle_diameter === 'string' ? o.nozzle_diameter : '',
				x: num(o.size_x), y: num(o.size_y), z: num(o.size_z), th: (th !== null && th >= 1) ? Math.round(th) : null,
				checked: typeof o.nozzle_selected === 'string' && o.nozzle_selected !== ''
			};
			r.hay = (brand + '\u0000' + o.vendor + '\u0000' + name).toLowerCase();
			rows.push(r);
			var key = o.vendor + '\n' + o.model;
			(byKey[key] = byKey[key] || []).push(r);
		}
		table.setRows(rows);
	}

	// What the page sends with "save_userguide_models": one entry per enabled model, keyed by
	// vendor + model, with all its nozzle variants (unchanged from the tile page, #345).
	function selection() {
		var rows = table.rows();
		var data = {}, count = 0;
		for (var i = 0; i < rows.length; i++) {
			var r = rows[i];
			if (!r.checked) continue;
			var key = r.vendor + '\n' + r.model;
			if (data.hasOwnProperty(key)) continue;
			data[key] = { model: r.model, nozzle_diameter: r.nozzles, vendor: r.vendor };
			count++;
		}
		return { count: count, data: data };
	}

	return {
		init: init,
		load: load,
		selection: selection,
		closePopover: table.closePopover,
		// for tests
		_state: table._state
	};
})();

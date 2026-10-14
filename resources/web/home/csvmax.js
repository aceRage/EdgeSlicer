// The "Max records per file" and "Rows" choices of the Vendors page's CSV export. No page code here, so
// tests/slic3rutils/vendor_csv_default_test.js can run it in plain node.
(function (root) {
  'use strict';
  var CAP = 1000000; // the same cap as the connector template's csv.max_records
  var PRESETS = [0, 100, 200, 500, 1000]; // 0 = All (one file)

  // A whole number of rows from 0 (All) to the cap; anything else is 0.
  function clean(v) {
    var n = typeof v === 'string' ? Number(v.trim() === '' ? NaN : v) : v;
    if (typeof n !== 'number' || !isFinite(n) || n < 1) return 0;
    return Math.min(Math.floor(n), CAP);
  }

  // What the control shows when the export opens.
  //   shown:  [{id, limit}] the connectors whose models are listed; limit is the connector's own
  //           csv.max_records (0 or missing = none)
  //   stored: {last: the user's last choice or null, byId: {connectorId: the user's choice for it}}
  // One connector: the user's choice for it, else its own limit, else the last choice. Several
  // connectors share one set of files, so the smallest limit among them wins (a connector the user
  // set to All counts as no limit), else the last choice. Nothing set anywhere: 0 = All.
  function defaultMax(shown, stored) {
    var st = stored || {};
    var byId = st.byId || {};
    var last = st.last === null || st.last === undefined ? 0 : clean(st.last);
    var smallest = 0;
    for (var k = 0; k < (shown || []).length; ++k) {
      var c = shown[k];
      var own = Object.prototype.hasOwnProperty.call(byId, c.id) ? clean(byId[c.id]) : clean(c.limit);
      if (own > 0 && (smallest === 0 || own < smallest)) smallest = own;
    }
    if (smallest > 0) return smallest;
    if ((shown || []).length === 1 && Object.prototype.hasOwnProperty.call(byId, shown[0].id)) return 0; // chose All for it
    return last;
  }

  // What one CSV row is: 'model' (the default) or 'file'. Anything else is '' (not set).
  var ROWS = ['model', 'file'];
  function cleanRows(v) { return v === 'model' || v === 'file' ? v : ''; }

  // The rows choice the export starts with; the same sources as defaultMax:
  //   shown:  [{id, rows}] the connector's own csv.rows ('' = none)
  //   stored: {rowsLast: the user's last choice, rowsById: {connectorId: the user's choice for it}}
  // The user's choice for a connector beats its own setting. When the connectors listed agree on one
  // value that is it; when none sets one, or they disagree, the last choice, else 'model'.
  function defaultRows(shown, stored) {
    var st = stored || {};
    var byId = st.rowsById || {};
    var last = cleanRows(st.rowsLast) || 'model';
    var seen = [];
    for (var k = 0; k < (shown || []).length; ++k) {
      var c = shown[k];
      var own = Object.prototype.hasOwnProperty.call(byId, c.id) ? cleanRows(byId[c.id]) : cleanRows(c.rows);
      if (own && seen.indexOf(own) < 0) seen.push(own);
    }
    return seen.length === 1 ? seen[0] : last;
  }

  var api = { CAP: CAP, PRESETS: PRESETS, ROWS: ROWS, clean: clean, defaultMax: defaultMax, cleanRows: cleanRows, defaultRows: defaultRows };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.CsvMax = api;
})(typeof window !== 'undefined' ? window : this);

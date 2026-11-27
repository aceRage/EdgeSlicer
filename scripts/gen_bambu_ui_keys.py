#!/usr/bin/env python3
"""Generate the list of settings Bambu Studio shows on its tabs, for the "Bambu" settings view.

The "Bambu" view (Basic | Advanced | Bambu selector, tests/design_view_modes.md) shows the options
Bambu Studio puts on its Process / Filament / Printer / Plate / Part tabs, on EdgeSlicer's own pages.
This script reads that list straight out of a Bambu Studio release tag, without checking it out:

    python scripts/gen_bambu_ui_keys.py --bambu C:/Dev/BambuStudio --tag v02.09.01.70 \
        --out src/libslic3r/Format/BambuUiKeys.cpp

Re-run it (and commit the result) in each Bambu port wave. `--report` prints the counts and the keys
that are new on the "missing" side instead of writing. The Bambu sources are only read as text
(`git show <tag>:<path>`); nothing is built or run, and no code is copied beyond key names.

What is parsed:
  * src/slic3r/GUI/Tab.cpp: every option key passed to append_single_option_line, get_option,
    create_single_option_line, create_line_with_widget, append_option_line,
    append_multi_nozzle_auto_line and the local lambdas that wrap them (append_nozzle_scaled_option,
    the filament override checkbox line, build_aux_fan_group), plus literal key lists looped into
    them, inside TabPrint::build, TabPrintModel::build, TabPrintPart::build, TabPrintPlate::build,
    TabFilament::build / add_filament_overrides_page, TabPrinter::build_fff / build_kinematics_page /
    build_unregular_pages / build_ams_filament_time_options. Comments, `#if 0` / `#else` branches
    of `#if 1` and `#ifdef HAS_PRESSURE_EQUALIZER` blocks (not built by Bambu) are dropped. The page
    and group of each key are the last add_options_page / new_optgroup title above it.
  * src/libslic3r/PrintConfig.cpp: the mode of each key (`def->mode`, comSimple when unset), including
    the loop-generated machine_max_* axis keys and the filament_* override keys.
  * src/slic3r/GUI/ConfigManipulation.cpp: the lines Bambu hides on every Bambu Lab printer
    (`if (is_BBL_Printer) { for (auto el : {...}) toggle_line(el, false); }`), applied by our Bambu
    view on Bambu Lab printers too (design decision D15).

Hand-maintained here (reviewed by the lead when the list is refreshed):
  * INFERRED_EQUIVALENTS: Bambu keys that are the same setting as one of ours under another name but
    are NOT in BambuKeyAliases (no value crosses over in 3MF files; the view only needs membership).
  * REVIEWED_MISSING: Bambu keys we do not have (features not ported). A Catch2 test
    (tests/libslic3r/test_bambu_ui_keys.cpp) fails when a listed key is neither ours, aliased, an
    inferred equivalent nor on this list, so a refresh that brings new Bambu settings is visible.
"""
import argparse
import re
import subprocess
import sys

TAB_FUNCTIONS = (
    ('process', 'TabPrint::build'),
    ('process', 'TabPrintModel::build'),
    ('part', 'TabPrintPart::build'),
    ('plate', 'TabPrintPlate::build'),
    ('filament', 'TabFilament::add_filament_overrides_page'),
    ('filament', 'TabFilament::build'),
    ('printer', 'TabPrinter::build_fff'),
    ('printer', 'TabPrinter::build_kinematics_page'),
    ('printer', 'TabPrinter::build_unregular_pages'),
    ('printer', 'TabPrinter::build_ams_filament_time_options'),
)
TABS = ('process', 'filament', 'printer', 'plate', 'part')

# Calls whose first string-literal argument (after an optional optgroup argument) is an option key.
KEY_CALLS = ('append_single_option_line', 'get_option', 'create_single_option_line', 'create_line_with_widget',
             'append_option_line', 'append_multi_nozzle_auto_line')

INFERRED_EQUIVALENTS = (
    # (Bambu key, our key, why)
    ('sub_top_surface_pattern', 'undertop_surface_pattern',
     'pattern of the solid layer under a top surface'),
)

# Bambu tab keys (Simple or Advanced) that EdgeSlicer does not have, reviewed 2026-11-24 against
# v02.09.01.70 (tests/design_view_modes_bambu_keys.json, "bambu_ui_keys_missing_in_edgeslicer").
# Porting them is separate work (design decision D14).
REVIEWED_MISSING = (
    'additional_fan_full_speed_layer', 'avoid_crossing_wall_includes_support',
    'close_additional_fan_first_x_layers', 'close_optional_fan_first_x_layers', 'conformal_hub_radius',
    'conformal_infill', 'conformal_link_flip_layers', 'conformal_link_keep_layers', 'conformal_pole',
    'conformal_ray_count', 'conformal_stagger', 'default_ams_type',
    'detect_floating_vertical_shell', 'embedding_wall_into_infill', 'enable_wrapping_detection',
    'filament_adaptive_volumetric_speed', 'filament_bridge_speed', 'filament_enable_overhang_speed',
    'filament_overhang_1_4_speed', 'filament_overhang_2_4_speed', 'filament_overhang_3_4_speed',
    'filament_overhang_4_4_speed', 'filament_overhang_totally_speed',
    'filament_scarf_gap', 'filament_scarf_height', 'filament_scarf_length', 'filament_scarf_seam_type',
    'filament_velocity_adaptation_factor', 'first_x_layer_fan_speed', 'first_x_layer_optional_fan_speed',
    'first_x_layer_part_fan_speed', 'infill_rotate_step', 'machine_hotend_change_time',
    'modifier_ignore_infill', 'optional_cooling_fan_speed', 'optional_fan_full_speed_layer',
    'overhang_threshold_participating_cooling', 'overhang_totally_speed',
    'override_filament_scarf_seam_setting', 'override_process_overhang_speed', 'periodic_modifier',
    'periodic_modifier_apply_layers', 'periodic_modifier_skip_layers', 'pre_start_fan_time',
    'seam_placement_away_from_overhangs', 'seam_slope_gap', 'smooth_coefficient',
    'smooth_speed_discontinuity_area', 'vertical_shell_speed', 'wrapping_detection_gcode',
    'z_direction_outwall_speed_continuous',
)


def git_show(repo, tag, path):
    return subprocess.check_output(['git', '-C', repo, 'show', '%s:%s' % (tag, path)]).decode('utf-8', 'replace')


def git_commit(repo, tag):
    return subprocess.check_output(['git', '-C', repo, 'rev-parse', '--short=9', tag + '^{commit}'], text=True).strip()


def strip_comments(text):
    """Drop // and /* */ comments, keeping string literals and line count."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def drop_dead_preprocessor(text):
    """Keep what Bambu builds: drop `#if 0` bodies, the `#else` of `#if 1`, and HAS_PRESSURE_EQUALIZER."""
    lines = text.split('\n')
    out = []
    stack = []  # each entry: [active_now, any_branch_taken, is_tracked]
    for line in lines:
        s = line.strip()
        m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', s)
        if m:
            d, arg = m.group(1), m.group(2).strip()
            if d in ('if', 'ifdef', 'ifndef'):
                if d == 'if' and arg in ('0', '1'):
                    stack.append([arg == '1', arg == '1', True])
                elif d == 'ifdef' and arg == 'HAS_PRESSURE_EQUALIZER':
                    stack.append([False, False, True])
                else:
                    stack.append([True, True, False])
            elif d == 'else' and stack:
                top = stack[-1]
                if top[2]:
                    top[0] = not top[1]
                    top[1] = True
            elif d == 'endif' and stack:
                stack.pop()
            out.append('')
            continue
        out.append(line if all(e[0] for e in stack) else '')
    return '\n'.join(out)


def function_body(src, qual_name):
    m = re.search(r'\n[\w:<>&* ]*\b%s\s*\([^)]*\)\s*(?:const\s*)?\n?\{' % re.escape(qual_name), src)
    if not m:
        sys.exit('function %s not found' % qual_name)
    depth, i = 1, m.end()
    while depth:
        c = src[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif c == '"':
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            i = j
        i += 1
    return src[m.end():i - 1], src.count('\n', 0, m.end()) + 1


def matching_paren(text, open_idx):
    depth, i = 0, open_idx
    while i < len(text):
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                return i
        elif c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            i = j
        i += 1
    return len(text) - 1


STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
LIT_LIST_RE = re.compile(r'(?:std::vector<std::string>|std::initializer_list<[^>]*>|auto)\s+(\w+)\s*=?\s*\{([^{}]*)\}')
FOR_LIT_RE = re.compile(r'for\s*\(\s*(?:const\s+)?[\w:&<>*\s]+?\b(\w+)\s*:\s*\{([^{}]*)\}\s*\)')
FOR_VAR_RE = re.compile(r'for\s*\(\s*(?:const\s+)?[\w:&<>*\s]+?\b(\w+)\s*:\s*(\w+)\s*\)')
LAMBDA_RE = re.compile(r'auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(')


def string_args(argtext):
    """String literals in an argument list that are not wrapped in L(...) / _(...)."""
    plain = re.sub(r'\b(?:L|_L|_)\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)', 'TITLE', argtext)
    return STR_RE.findall(plain)


def title_of(argtext):
    m = re.match(r'\s*(?:L|_L|_)?\s*\(?\s*"((?:[^"\\]|\\.)*)"', argtext)
    return m.group(1) if m else ''


def keys_in_function(body, first_line, defined, extra_lists):
    """[(key, page, group, line)] in source order."""
    rows = []
    # Local and file-level literal lists, for `for (x : name)` loops.
    lists = dict(extra_lists)
    for m in LIT_LIST_RE.finditer(body):
        lists[m.group(1)] = STR_RE.findall(m.group(2))
    # Local lambdas that forward string arguments to a key call.
    lambdas = {}
    for m in LAMBDA_RE.finditer(body):
        close = matching_paren(body, m.end() - 1)
        brace = body.find('{', close)
        end = matching_paren(body, brace)
        lbody = body[brace:end]
        if any(c + '(' in lbody for c in KEY_CALLS):
            lambdas[m.group(1)] = (m.start(), end, 'new_optgroup(' in lbody)
    # Loop variables bound to literal lists: name -> (list, start, end of the loop statement).
    loops = []
    for m in FOR_LIT_RE.finditer(body):
        loops.append((m.group(1), STR_RE.findall(m.group(2)), m.end()))
    for m in FOR_VAR_RE.finditer(body):
        if m.group(2) in lists:
            loops.append((m.group(1), lists[m.group(2)], m.end()))

    def loop_values(var, pos):
        best = None
        for name, vals, start in loops:
            if name == var and start <= pos and (best is None or start > best[1]):
                best = (vals, start)
        return best[0] if best else []

    events = []
    for m in re.finditer(r'\badd_options_page\s*\(', body):
        title = title_of(body[m.end():])
        vm = re.match(r'\s*(\w+)\s*,', body[m.end():])
        if not title and vm:
            # A title held in a variable: the last plain literal of its assignment
            # (`page_name = n > 1 ? Format("Extruder %d", n) : Format("Extruder")` -> "Extruder").
            assigns = list(re.finditer(r'\b%s\s*=([^;]*);' % vm.group(1), body[:m.start()]))
            if assigns:
                lits = [x for x in STR_RE.findall(assigns[-1].group(1)) if '%' not in x]
                title = lits[-1] if lits else ''
        events.append((m.start(), 'page', title))
    for m in re.finditer(r'\bnew_optgroup\s*\(', body):
        events.append((m.start(), 'group', title_of(body[m.end():])))
    calls = KEY_CALLS + tuple(lambdas)
    for m in re.finditer(r'\b(%s)\s*\(' % '|'.join(calls), body):
        name = m.group(1)
        if name in lambdas:
            lstart, lend, _ = lambdas[name]
            if lstart <= m.start() < lend:
                continue  # the lambda's own body
        else:
            if any(ls <= m.start() < le for ls, le, _ in lambdas.values()):
                continue  # inside a lambda body: keys come from its call sites
        close = matching_paren(body, m.end() - 1)
        args = body[m.end():close]
        keys = []
        if name in lambdas:
            keys = string_args(args)
            if not keys:
                vm = re.match(r'\s*(\w+)\s*[,)]', args + ')')
                if vm:
                    keys = loop_values(vm.group(1), m.start())
            if lambdas[name][2]:
                events.append((m.start(), 'group', title_of(args)))
        else:
            lits = string_args(args)
            if lits:
                # `"prefix_" + var` over a literal list
                cm = re.search(r'"((?:[^"\\]|\\.)*)"\s*\+\s*(\w+)', args)
                if cm and cm.group(1) == lits[0]:
                    keys = [cm.group(1) + v for v in loop_values(cm.group(2), m.start())]
                else:
                    keys = [lits[0]]
            else:
                vm = re.match(r'\s*(?:[\w.>()-]+\s*,\s*)?(\w+)\s*[,)]', args + ')')
                if vm:
                    keys = loop_values(vm.group(1), m.start())
        for k in keys:
            events.append((m.start(), 'key', k))
    events.sort(key=lambda e: e[0])
    page = group = ''
    for pos, kind, val in events:
        if kind == 'page':
            page, group = val, ''
        elif kind == 'group':
            group = val
        elif val in defined:
            rows.append((val, page, group, first_line + body.count('\n', 0, pos)))
    return rows


def parse_modes(src):
    """key -> 'simple' | 'advanced' | 'develop' from PrintConfigDef."""
    src = strip_comments(src)
    modes = {}
    lists = {m.group(1): STR_RE.findall(m.group(2))
             for m in re.finditer(r'const\s+std::vector<std::string>\s+(\w+)\s*=\s*\{(.*?)\};', src, re.S)}
    axes = []
    am = re.search(r'std::vector<AxisDefault>\s+axes\s*\{(.*?)\};', src, re.S)
    if am:
        axes = re.findall(r'\{\s*"(\w+)"', am.group(1))
    adds = list(re.finditer(r'this->add(?:_nullable)?\(\s*("(?:[^"\\]|\\.)*"(?:\s*\+\s*[\w.]+)?|\w+)\s*,', src))
    for i, m in enumerate(adds):
        end = adds[i + 1].start() if i + 1 < len(adds) else len(src)
        seg = src[m.end():end]
        mm = re.findall(r'def->mode\s*=\s*com(\w+)', seg)
        mode = (mm[0] if mm else 'Simple').lower()
        arg = m.group(1)
        if arg.startswith('"'):
            lit = STR_RE.match(arg).group(1)
            if '+' in arg:
                for a in axes:
                    modes.setdefault(lit + a, mode)
            else:
                modes.setdefault(lit, mode)
            continue
        # `for (auto& opt_key : <list>) { def = this->add_nullable(opt_key, ...) ... }`
        loop = None
        for lm in re.finditer(r'for\s*\(\s*auto\s*&\s*(\w+)\s*:\s*(\w+)\s*\)', src[:m.start()]):
            loop = lm
        if not loop or loop.group(1) != arg or loop.group(2) not in lists:
            continue
        simple = set(re.findall(r'%s\s*==\s*"(\w+)"' % arg, seg.split('else')[0])) if 'comSimple' in seg else set()
        for k in lists[loop.group(2)]:
            if simple:
                modes.setdefault(k, 'simple' if k in simple else 'advanced')
            else:
                modes.setdefault(k, mode)
    return modes


def parse_bbl_hidden(src):
    src = strip_comments(src)
    hidden = []
    for m in re.finditer(r'if\s*\(\s*is_BBL_Printer\s*\)\s*\{', src):
        block = src[m.end():matching_paren(src, m.end() - 1)]
        for fm in re.finditer(r'for\s*\([^:]*:\s*\{([^{}]*)\}\s*\)\s*toggle_line\s*\(\s*\w+\s*,\s*false\s*\)', block):
            hidden += [k for k in STR_RE.findall(fm.group(1)) if k not in hidden]
    return hidden


def collect(repo, tag):
    tab_src = drop_dead_preprocessor(strip_comments(git_show(repo, tag, 'src/slic3r/GUI/Tab.cpp')))
    modes = parse_modes(git_show(repo, tag, 'src/libslic3r/PrintConfig.cpp'))
    file_lists = {m.group(1): STR_RE.findall(m.group(2))
                  for m in re.finditer(r'static\s+const\s+std::vector<std::string>\s+(\w+)\s*=\s*\{([^{}]*)\}', tab_src)}
    rows, seen = [], set()
    for tab, fn in TAB_FUNCTIONS:
        body, first_line = function_body(tab_src, fn)
        for key, page, group, line in keys_in_function(body, first_line, modes, file_lists):
            if (tab, key) in seen:
                continue
            seen.add((tab, key))
            rows.append({'tab': tab, 'key': key, 'mode': modes[key], 'page': page, 'group': group, 'line': line})
    bbl_hidden = parse_bbl_hidden(git_show(repo, tag, 'src/slic3r/GUI/ConfigManipulation.cpp'))
    return rows, bbl_hidden


def c_str(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def emit_cpp(rows, bbl_hidden, version, commit, tag, out):
    lines = [
        '// GENERATED FILE - do not edit by hand.',
        '// Source: Bambu Studio %s (tag %s, commit %s): src/slic3r/GUI/Tab.cpp, src/libslic3r/PrintConfig.cpp,' % (version, tag, commit),
        '//         src/slic3r/GUI/ConfigManipulation.cpp',
        '// Generator: scripts/gen_bambu_ui_keys.py (design: tests/design_view_modes.md)',
        '#include "BambuUiKeys.hpp"',
        '',
        'namespace Slic3r {',
        'namespace BambuUiKeys {',
        '',
        'const char *bambu_version() { return %s; }' % c_str(version),
        'const char *bambu_commit() { return %s; }' % c_str(commit),
        '',
        '// Every option line on Bambu Studio\'s tabs: Bambu key, tab, the Bambu view level that shows it,',
        '// Bambu page and group (for reference; the Bambu view keeps EdgeSlicer\'s own layout), Tab.cpp line.',
        'const std::vector<Entry> &entries()',
        '{',
        '    static const std::vector<Entry> list = {',
    ]
    for r in rows:
        lines.append('        { %s, Tab::%s, Level::%s, %s, %s, %d },' % (
            c_str(r['key']), r['tab'].capitalize(), r['mode'].capitalize(), c_str(r['page']), c_str(r['group']), r['line']))
    lines += ['    };', '    return list;', '}', '']
    lines += ['// Bambu keys that are the same setting as one of ours under another name, not in BambuKeyAliases.',
              'const std::vector<InferredEquivalent> &inferred_equivalents()', '{',
              '    static const std::vector<InferredEquivalent> list = {']
    for b, o, why in INFERRED_EQUIVALENTS:
        lines.append('        { %s, %s }, // %s' % (c_str(b), c_str(o), why))
    lines += ['    };', '    return list;', '}', '']
    lines += ['// Bambu keys (Simple or Advanced) EdgeSlicer does not have; reviewed list, see the generator.',
              'const std::vector<const char *> &reviewed_missing()', '{', '    static const std::vector<const char *> list = {']
    for k in REVIEWED_MISSING:
        lines.append('        %s,' % c_str(k))
    lines += ['    };', '    return list;', '}', '']
    lines += ['// Lines Bambu Studio hides on every Bambu Lab printer (ConfigManipulation.cpp, is_BBL_Printer).',
              'const std::vector<const char *> &bbl_printer_hidden()', '{', '    static const std::vector<const char *> list = {']
    for k in bbl_hidden:
        lines.append('        %s,' % c_str(k))
    lines += ['    };', '    return list;', '}', '', '} // namespace BambuUiKeys', '} // namespace Slic3r', '']
    with open(out, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bambu', required=True, help='Bambu Studio git clone (read with git show, never checked out)')
    ap.add_argument('--tag', required=True, help='Bambu Studio release tag, e.g. v02.09.01.70')
    ap.add_argument('--out', help='generated C++ file to write (src/libslic3r/Format/BambuUiKeys.cpp)')
    ap.add_argument('--report', action='store_true', help='print counts instead of writing')
    args = ap.parse_args()

    rows, bbl_hidden = collect(args.bambu, args.tag)
    version = args.tag.lstrip('v')
    commit = git_commit(args.bambu, args.tag)
    if args.report or not args.out:
        counts = {}
        for r in rows:
            counts.setdefault(r['tab'], {}).setdefault(r['mode'], 0)
            counts[r['tab']][r['mode']] += 1
        for t in TABS:
            print(t, counts.get(t, {}))
        print('total', len(rows), 'simple+advanced', sum(1 for r in rows if r['mode'] != 'develop'))
        print('hidden on Bambu Lab printers:', ' '.join(bbl_hidden))
        if not args.out:
            return
    emit_cpp(rows, bbl_hidden, version, commit, args.tag, args.out)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Generate the Bambu Studio schema table used by "Export Bambu 3MF".

Bambu Studio's project loader (PrintConfigDef::handle_legacy) silently drops every key its
print_config_def does not declare, and ConfigOption::deserialize() rejects values whose shape does
not fit the declared type. The Bambu export therefore needs Bambu's option list, with types,
nullability, accepted enum values and the per-extruder-variant class of each option. This script
reads all of it straight out of a Bambu Studio source checkout instead of hand-maintaining a list:

    python scripts/gen_bambu_known_keys.py --bambu C:/Dev/BambuStudio \
        --ours src/libslic3r/PrintConfig.cpp --out src/libslic3r/Format/BambuKnownKeys.cpp

Re-run it (and commit the result) whenever the export should target a newer Bambu Studio, or when
one of our enum options gains a value. `--compare` prints a key/type/enum diff instead of writing.

What is parsed (PrintConfigDef::init_common_params / init_fff_params only; the CLI and SLA defs
are not part of a project config):
  * `this->add("key", coType)` and `this->add_nullable("key", coType)`, `def->nullable = true`
  * the loop-generated `machine_max_{speed,acceleration,jerk}_{x,y,z,e}` family
  * the `filament_extruder_override_keys` / `filament_overhang_override_keys` loops, which add
    nullable `filament_<key>` options typed like `<key>`
  * enum values: the option's `enum_keys_map` (`&ConfigOptionEnum<X>::get_enum_values()` or
    `&s_keys_map_X`), which is exactly what deserialize() accepts, falling back to the
    `enum_values` pushes when an option has no keys map
  * the keys Bambu's own handle_legacy() throws away on load (its `ignore` set): writing them is
    pointless, so they are left out of the known list
  * the per-variant option sets (print_options_with_variant, filament_options_with_variant,
    printer_options_with_variant_1/_2, printer_extruder_options), which decide how long Bambu
    expects each vector to be

Enum translation (ours -> Bambu) is derived by matching enumerator names: our "rectilinear" is
ipRectilinear, which Bambu spells "zig-zag". Values with no Bambu enumerator are emitted with an
empty target, meaning "Bambu cannot represent this; leave the key out".
"""
import argparse
import os
import re
import subprocess
import sys

ADD_RE = re.compile(
    r'(?:(\w+)\s*=\s*)?def\s*=\s*this->(add|add_nullable)\(\s*'
    r'("(?:[^"\\]|\\.)*"(?:\s*\+\s*[\w.]+)?|[\w.]+)\s*,\s*(co\w+)\s*\)')
KEYS_MAP_RE = re.compile(r'static\s+(?:const\s+)?t_config_enum_values\s+s_keys_map_(\w+)\s*=?\s*\{(.*?)\n\};', re.S)
KEYS_MAP_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*([^}]+?)\s*\}')
VECTOR_LIST_RE = r'(?:const\s+)?std::vector<std::string>\s+%s\s*=\s*\{(.*?)\};'
SET_LIST_RE = r'std::set<std::string>\s+%s\s*=\s*\{(.*?)\};'

VARIANT_SETS = (
    ('print_options_with_variant', 'Print'),
    ('filament_options_with_variant', 'Filament'),
    ('printer_options_with_variant_1', 'Printer'),
    ('printer_options_with_variant_2', 'PrinterPair'),
    ('printer_extruder_options', 'Extruder'),
)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def function_body(src, name):
    """Text of `void PrintConfigDef::<name>()`; functions in this file end with a column-0 brace."""
    m = re.search(r'\nvoid PrintConfigDef::%s\([^)]*\)\s*\n\{' % name, src)
    if not m:
        return ''
    end = src.find('\n}\n', m.end())
    return src[m.end():end]


def enum_constant(expr):
    """`int(FuzzySkinType::None)` / `ipRectilinear` -> the bare enumerator name."""
    expr = expr.strip()
    m = re.match(r'^int\s*\((.*)\)$', expr)
    if m:
        expr = m.group(1)
    return expr.split('::')[-1].strip()


def parse_keys_maps(src):
    """{enum class: [(string, enumerator), ...]} from every s_keys_map_<Class> literal."""
    maps = {}
    for m in KEYS_MAP_RE.finditer(src):
        body = strip_comments(m.group(2))
        maps[m.group(1)] = [(k, enum_constant(c)) for k, c in KEYS_MAP_ENTRY_RE.findall(body)]
    return maps


def parse_string_list(src, regex, name):
    m = re.search(regex % name, src, re.S)
    if not m:
        return []
    return re.findall(r'"([^"]+)"', strip_comments(m.group(1)))


def parse_legacy_ignore(src):
    body = function_body(src, 'handle_legacy')
    m = re.search(r'static\s+std::set<std::string>\s+ignore\s*=\s*\{(.*?)\};', body, re.S)
    if not m:
        return set()
    return set(re.findall(r'"([^"]+)"', strip_comments(m.group(1))))


def parse_defs(src):
    maps = parse_keys_maps(src)
    body = function_body(src, 'init_common_params') + '\n' + function_body(src, 'init_fff_params')
    options = {}   # key -> dict(type, nullable, enum, enum_map)
    aliases = {}   # C++ variable name -> key (e.g. def_top_fill_pattern)

    matches = list(ADD_RE.finditer(body))
    for i, m in enumerate(matches):
        alias, kind, key_expr, typ = m.groups()
        block = body[m.end(): matches[i + 1].start() if i + 1 < len(matches) else len(body)]
        if key_expr.startswith('"') and '+' not in key_expr:
            keys = [key_expr.strip('"')]
        elif key_expr.startswith('"') and 'axis.name' in key_expr:
            prefix = key_expr.split('"')[1]
            keys = [prefix + a for a in ('x', 'y', 'z', 'e')]
        elif key_expr == 'opt_key':
            continue  # override-key loops, handled below
        else:
            print('note: skipped computed key expression %r' % key_expr, file=sys.stderr)
            continue
        nullable = kind == 'add_nullable' or re.search(r'def->nullable\s*=\s*true', block) is not None
        is_enum = typ in ('coEnum', 'coEnums')
        enum = enum_map = None
        km = re.search(r'def->enum_keys_map\s*=\s*&(?:ConfigOptionEnum<(\w+)>::get_enum_values\(\)|s_keys_map_(\w+))', block)
        if km:
            enum_map = maps.get(km.group(1) or km.group(2))
            enum = [k for k, _ in enum_map] if enum_map else None
        if enum is None:
            vals = re.findall(r'def->enum_values\.(?:push_back|emplace_back)\(\s*"([^"]*)"', block)
            copy = re.search(r'def->enum_values\s*=\s*(\w+)->enum_values', block)
            if copy and copy.group(1) in aliases:
                vals = options[aliases[copy.group(1)]]['enum'] or vals
            enum = vals or None
        for k in keys:
            options[k] = {'type': typ, 'nullable': nullable,
                          'enum': enum if is_enum else None, 'enum_map': enum_map if is_enum else None}
            if alias:
                aliases[alias] = k

    # filament_<key> overrides: same type as <key>, always nullable.
    for list_name in ('filament_extruder_override_keys', 'filament_overhang_override_keys'):
        for fk in parse_string_list(src, VECTOR_LIST_RE, list_name):
            base = fk[len('filament_'):] if fk.startswith('filament_') else fk
            if base not in options:
                print('warning: %s: base option %s of %s not found' % (list_name, base, fk), file=sys.stderr)
                continue
            options[fk] = dict(options[base], nullable=True)
    return options


def variant_classes(src):
    out = {}
    for set_name, cls in VARIANT_SETS:
        for k in parse_string_list(src, SET_LIST_RE, set_name):
            out.setdefault(k, cls)
    return out


def enum_translations(ours, bambu):
    """[(key, our value, bambu value or '')] for every enum value of ours Bambu does not accept as-is."""
    rows = []
    for k in sorted(set(ours) & set(bambu)):
        o, b = ours[k], bambu[k]
        if not o['enum'] or not b['enum'] or o['type'] not in ('coEnum', 'coEnums'):
            continue
        accepted = set(b['enum'])
        by_const = {c: s for s, c in (b['enum_map'] or [])}
        for value, const in (o['enum_map'] or [(v, None) for v in o['enum']]):
            if value in accepted:
                continue
            rows.append((k, value, by_const.get(const, '') if const else ''))
    return rows


def git_head(repo):
    try:
        return subprocess.check_output(['git', '-C', repo, 'rev-parse', '--short=10', 'HEAD'], text=True).strip()
    except Exception:
        return 'unknown'


def bambu_version(repo):
    m = re.search(r'set\(SLIC3R_VERSION\s+"([^"]+)"\)', read(os.path.join(repo, 'version.inc')))
    return m.group(1) if m else 'unknown'


def c_str(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def emit_cpp(options, ignored, variants, translations, version, head, out):
    L = []
    L.append('// GENERATED FILE - do not edit by hand.')
    L.append('// Source: Bambu Studio %s (commit %s), src/libslic3r/PrintConfig.cpp' % (version, head))
    L.append('// Generator: scripts/gen_bambu_known_keys.py (see docs/bambu-3mf-export.md)')
    L.append('#include "BambuExport.hpp"')
    L.append('')
    L.append('namespace Slic3r {')
    L.append('namespace BambuExport {')
    L.append('')
    L.append('const char *bambu_reference_version() { return %s; }' % c_str(version))
    L.append('const char *bambu_reference_commit() { return %s; }' % c_str(head))
    L.append('')
    L.append('// Every option Bambu Studio keeps when it loads a project: key, Bambu type, nullable,')
    L.append('// per-extruder variant class, accepted enum values joined by "|" (enum options only).')
    L.append('// Keys Bambu declares but discards in its own handle_legacy() are not listed:')
    L.append('//   %s' % ' '.join(sorted(ignored)))
    L.append('const std::vector<BambuKeyDef> &bambu_key_defs()')
    L.append('{')
    L.append('    static const std::vector<BambuKeyDef> defs = {')
    for k in sorted(options):
        if k in ignored:
            continue
        o = options[k]
        enum = 'nullptr' if not o['enum'] else c_str('|'.join(o['enum']))
        L.append('        { %s, %s, %s, VariantClass::%s, %s },' % (
            c_str(k), c_str(o['type']), 'true' if o['nullable'] else 'false', variants.get(k, 'None'), enum))
    L.append('    };')
    L.append('    return defs;')
    L.append('}')
    L.append('')
    L.append('// Our enum values Bambu does not accept as written: key, our value, Bambu value.')
    L.append('// Matched by enumerator name; an empty Bambu value means Bambu has no such enumerator.')
    L.append('const std::vector<EnumTranslation> &generated_enum_translations()')
    L.append('{')
    L.append('    static const std::vector<EnumTranslation> rows = {')
    for k, ov, bv in translations:
        L.append('        { %s, %s, %s },' % (c_str(k), c_str(ov), c_str(bv)))
    L.append('    };')
    L.append('    return rows;')
    L.append('}')
    L.append('')
    L.append('} // namespace BambuExport')
    L.append('} // namespace Slic3r')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    print('wrote %d keys, %d enum translations to %s' % (len(options) - len(ignored & set(options)), len(translations), out))


def compare(ours, bambu, ignored):
    only_ours = sorted(set(ours) - set(bambu))
    only_bambu = sorted(set(bambu) - set(ours))
    print('bambu keys: %d (%d ignored on load), our keys: %d' % (len(bambu), len(ignored & set(bambu)), len(ours)))
    print('\n== only ours (%d)' % len(only_ours))
    for k in only_ours:
        print('  %s %s' % (k, ours[k]['type']))
    print('\n== only bambu (%d)' % len(only_bambu))
    for k in only_bambu:
        print('  %s %s' % (k, bambu[k]['type']))
    print('\n== type differs')
    for k in sorted(set(ours) & set(bambu)):
        a, b = ours[k], bambu[k]
        if a['type'] != b['type'] or a['nullable'] != b['nullable']:
            print('  %-45s ours %s%s  bambu %s%s' % (k, a['type'], ' nullable' if a['nullable'] else '',
                                                    b['type'], ' nullable' if b['nullable'] else ''))
    print('\n== enum translations (ours -> bambu, empty = unsupported)')
    for k, ov, bv in enum_translations(ours, bambu):
        print('  %-40s %-25s -> %s' % (k, ov, bv or '(none)'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bambu', required=True, help='Bambu Studio source checkout')
    ap.add_argument('--ours', required=True, help='our src/libslic3r/PrintConfig.cpp (for enum translation)')
    ap.add_argument('--out', help='generated C++ file to write')
    ap.add_argument('--compare', action='store_true', help='print a key/type/enum diff instead of writing')
    args = ap.parse_args()

    bsrc = read(os.path.join(args.bambu, 'src', 'libslic3r', 'PrintConfig.cpp'))
    bambu = parse_defs(bsrc)
    ours = parse_defs(read(args.ours))
    ignored = parse_legacy_ignore(bsrc)
    if args.compare:
        compare(ours, bambu, ignored)
        return
    if not args.out:
        ap.error('--out or --compare is required')
    emit_cpp(bambu, ignored, variant_classes(bsrc), enum_translations(ours, bambu),
             bambu_version(args.bambu), git_head(args.bambu), args.out)


if __name__ == '__main__':
    main()

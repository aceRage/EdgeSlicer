"""Reads EdgeSlicer's own source for the printer-protocol fields it READS (JSON keys it looks up in
a printer's answers) and SENDS (command fields, form fields, macros, endpoints), per emulator
personality. Used by test_conformance_ledger.py; run alone it prints what it finds:

    python tests/emulator/conformance_scan.py [personality]

It is a text scan, not a C++ parser: it finds string literals in the places EdgeSlicer reads JSON
(`contains("k")`, `value("k", ..)`, `j["k"]` not followed by an assignment, `str_of(x, "k")`-style
helpers, boost ptree `get_optional<..>("A.B")`) and in the places it builds commands. A key built
at run time ("filament_slot" + n) is reported as "filament_slot<n>".
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEDGER_DIR = os.path.join(REPO, "tools", "printer_emulator", "conformance")

KEY = r'"([A-Za-z_][\w .]*)"'
READ_PATTERNS = [
    re.compile(r'\b(?:contains|value|count|get_child_optional|get_child|get_optional<[^>]*>|get<[^>]*>)\s*\(\s*' + KEY),
    re.compile(r'\b\w+(?:_of|_at|_str|_int|_num|_bool)\s*\(\s*[^,()]*,\s*' + KEY),
    re.compile(r'\[\s*' + KEY + r'\s*\](?!\s*=[^=])'),
]
DYNAMIC = re.compile(r'"([A-Za-z_]+)"\s*\+\s*std::to_string')
SEND_ASSIGN = re.compile(r'\[\s*"([A-Za-z_]\w*)"\s*\]\s*=[^=]')
FORM = re.compile(r'form_add(?:_file)?\(\s*"([^"]+)"')
CMD_CONST = re.compile(r'\b(?:CMD|ELEGOO)_[A-Z_]+\s*=\s*(\d+)')
CMD_ASSIGN = re.compile(r'\["Cmd"\]\s*=\s*(\d+)')
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
MACRO = re.compile(r'\b((?:SET|SDCARD|SM|INNER|PARK|SAVE)_[A-Z0-9_]+)\b')
GCODE = re.compile(r'^([GM]\d{1,3})\b')
PARAM = re.compile(r'\b([A-Z][A-Z0-9_]{2,})(?==)')
ENDPOINT = re.compile(r'^(/(?:server|printer|machine|access|api|uploadFile)/[\w/.]+)')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def read_source(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8", errors="replace") as f:
        return strip_comments(f.read())


def load_ledger(personality):
    with open(os.path.join(LEDGER_DIR, personality + ".json"), encoding="utf-8") as f:
        return json.load(f)


def scan_reads(sources):
    found = {}
    for rel in sources:
        text = read_source(rel)
        for pat in READ_PATTERNS:
            for m in pat.finditer(text):
                found.setdefault(m.group(1), set()).add(os.path.basename(rel))
        for m in DYNAMIC.finditer(text):
            found.setdefault(m.group(1) + "<n>", set()).add(os.path.basename(rel))
    return found


def scan_sends(spec):
    """spec: {"assign": [files], "form": [files], "cmd": [files], "macros": [files], "endpoints": [files]}"""
    found = {}

    def add(k, rel):
        found.setdefault(k, set()).add(os.path.basename(rel))

    for rel in spec.get("assign", []):
        for m in SEND_ASSIGN.finditer(read_source(rel)):
            add(m.group(1), rel)
    for rel in spec.get("form", []):
        for m in FORM.finditer(read_source(rel)):
            add("form:" + m.group(1), rel)
    for rel in spec.get("cmd", []):
        text = read_source(rel)
        for m in list(CMD_CONST.finditer(text)) + list(CMD_ASSIGN.finditer(text)):
            add("Cmd %s" % m.group(1), rel)
    for rel in spec.get("macros", []):
        for s in STRING.finditer(read_source(rel)):
            lit = s.group(1)
            for m in MACRO.finditer(lit):
                add(m.group(1), rel)
            for m in PARAM.finditer(lit):
                add(m.group(1) + "=", rel)
            m = GCODE.match(lit)
            if m:
                add(m.group(1), rel)
    for rel in spec.get("endpoints", []):
        for s in STRING.finditer(read_source(rel)):
            m = ENDPOINT.match(s.group(1))
            if m:
                add(m.group(1).rstrip("/"), rel)
    return found


def main():
    which = sys.argv[1:] or [n[:-5] for n in sorted(os.listdir(LEDGER_DIR)) if n.endswith(".json")]
    for p in which:
        led = load_ledger(p)
        print("== %s reads ==" % p)
        for k, files in sorted(scan_reads(led["read_sources"]).items()):
            print("  %-34s %-16s %s" % (k, (led["reads"].get(k) or {}).get("status", "UNLISTED"), ", ".join(sorted(files))))
        print("== %s sends ==" % p)
        for k, files in sorted(scan_sends(led["send_sources"]).items()):
            print("  %-34s %-16s %s" % (k, (led["sends"].get(k) or {}).get("status", "UNLISTED"), ", ".join(sorted(files))))


if __name__ == "__main__":
    main()

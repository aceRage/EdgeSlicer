"""Klipper's G-code line parsing and SAVE_VARIABLE semantics, written from the protocol description
(Klipper docs: G-Codes, "Extended G-Code Commands" and [save_variables]), not from EdgeSlicer's
code (design section 8: never share the product's parsers).

Klipper, per line of a script:
  * a comment starts at ';' (and, for extended commands, at '#' or '*');
  * a "traditional" command is a letter and a number (G1, M104, G31) followed by letter+value
    words (X10 S200);
  * an extended command (SAVE_VARIABLE, SET_LED ...) takes KEY=VALUE arguments split with shell
    quoting rules (shlex), keys upper-cased, values kept as written;
  * SAVE_VARIABLE VARIABLE=<name> VALUE=<python literal>: the name must be lower case, the value
    goes through ast.literal_eval, so VALUE="'slot3'" stores the string slot3 and VALUE=1 the int 1;
  * the first line that fails stops the script; the HTTP call answers 400 with the error.
"""
import ast
import re
import shlex

EXT_RE = re.compile(r"^\s*(?:N[0-9]+\s*)?(?P<cmd>[a-zA-Z_][a-zA-Z0-9_]+)(?:\s+|$)(?P<args>[^#*;]*?)\s*(?:[#*;].*)?$")
TRAD_CMD_RE = re.compile(r"^[A-Za-z][0-9]+(?:\.[0-9]+)?$")
TRAD_WORD_RE = re.compile(r"([A-Za-z])\s*([^A-Za-z\s]*)")


class GcodeError(Exception):
    pass


def split_script(script):
    return [ln for ln in (script or "").replace("\r\n", "\n").split("\n")]


def parse_line(line):
    """-> (COMMAND, {PARAM: value}) or None for a blank / comment-only line. GcodeError when the
    line is malformed."""
    text = line.strip()
    if not text or text.startswith(";") or text.startswith("#"):
        return None
    first = text.split(None, 1)[0]
    if TRAD_CMD_RE.match(first.split(";", 1)[0]):
        body = text.split(";", 1)[0]
        words = TRAD_WORD_RE.findall(body)
        if not words:
            return None
        cmd = (words[0][0] + words[0][1]).upper()
        params = {w[0].upper(): w[1] for w in words[1:]}
        return cmd, params
    m = EXT_RE.match(text)
    if not m:
        raise GcodeError("Malformed command '%s'" % text)
    cmd = m.group("cmd").upper()
    args = m.group("args") or ""
    try:
        parts = shlex.split(args)
    except ValueError:
        raise GcodeError("Malformed command '%s'" % text)
    params = {}
    for p in parts:
        if "=" not in p:
            raise GcodeError("Malformed command '%s'" % text)
        k, v = p.split("=", 1)
        params[k.upper()] = v
    return cmd, params


def save_variable_args(params):
    """SAVE_VARIABLE's (name, value, raw value). GcodeError the way Klipper words it."""
    if "VARIABLE" not in params:
        raise GcodeError("Error on 'SAVE_VARIABLE': missing VARIABLE")
    name = params["VARIABLE"]
    if name.lower() != name:
        raise GcodeError("VARIABLE must not contain upper case")
    if "VALUE" not in params:
        raise GcodeError("Error on 'SAVE_VARIABLE': missing VALUE")
    raw = params["VALUE"]
    try:
        value = ast.literal_eval(raw)
    except (ValueError, SyntaxError):
        raise GcodeError("Unable to parse '%s' as a literal" % raw)
    return name, value, raw

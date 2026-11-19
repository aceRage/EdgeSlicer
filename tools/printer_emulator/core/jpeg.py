"""A camera test pattern as baseline JPEG, standard library only (no Pillow): colour bars, the
printer's name, a clock and a block that moves once a second, so a viewer can tell the picture is
live and whose it is.

The encoder is the textbook one (ITU T.81 baseline, YCbCr 4:4:4, the Annex K quantisation and
Huffman tables). A block of one flat colour - most of the pattern - is encoded from its DC term
alone, so a 320x240 frame takes a fraction of a second in pure Python.
"""
import math
import time

W, H = 320, 240

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60,
          61, 54, 47, 55, 62, 63]

Q_LUM = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,
         51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121,
         120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
Q_CHR = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99,
         99, 99, 99, 99] + [99] * 32

DC_LUM_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_CHR_BITS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
DC_VALS = list(range(12))
AC_LUM_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D]
AC_LUM_VALS = bytes.fromhex(
    "01020300041105122131410613516107227114328191a1082342b1c11552d1f0"
    "2433627282090a161718191a25262728292a3435363738393a434445464748494a"
    "535455565758595a636465666768696a737475767778797a838485868788898a"
    "92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7"
    "c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa")
AC_CHR_BITS = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77]
AC_CHR_VALS = bytes.fromhex(
    "000102031104052131061241510761711322328108144291a1b1c109233352f0"
    "156272d10a162434e125f11718191a262728292a35363738393a434445464748"
    "494a535455565758595a636465666768696a737475767778797a828384858687"
    "88898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3"
    "c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa")
assert len(AC_LUM_VALS) == 162 and len(AC_CHR_VALS) == 162


def _huff_codes(bits, vals):
    codes, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


DC_LUM = _huff_codes(DC_LUM_BITS, DC_VALS)
DC_CHR = _huff_codes(DC_CHR_BITS, DC_VALS)
AC_LUM = _huff_codes(AC_LUM_BITS, list(AC_LUM_VALS))
AC_CHR = _huff_codes(AC_CHR_BITS, list(AC_CHR_VALS))

_COS = [[math.cos((2 * x + 1) * u * math.pi / 16) * (math.sqrt(0.5) if u == 0 else 1.0) / 2 for x in range(8)]
        for u in range(8)]


class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code, length):
        self.acc = (self.acc << length) | (code & ((1 << length) - 1))
        self.n += length
        while self.n >= 8:
            self.n -= 8
            b = (self.acc >> self.n) & 0xFF
            self.out.append(b)
            if b == 0xFF:
                self.out.append(0)
        self.acc &= (1 << self.n) - 1 if self.n else 0

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


def _category(v):
    v = abs(v)
    n = 0
    while v:
        n += 1
        v >>= 1
    return n


def _encode_block(bits, block, q, prev_dc, dc_codes, ac_codes):
    """block: 64 samples (0..255), row-major. Returns the block's DC for the next one's diff."""
    first = block[0]
    if all(v == first for v in block):
        coef = [0] * 64
        coef[0] = int(round((first - 128) * 8 / q[0]))
    else:
        s = [v - 128 for v in block]
        tmp = [0.0] * 64
        for y in range(8):
            row = s[y * 8:y * 8 + 8]
            for u in range(8):
                c = _COS[u]
                tmp[y * 8 + u] = (row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3] * c[3] + row[4] * c[4]
                                  + row[5] * c[5] + row[6] * c[6] + row[7] * c[7])
        coef = [0] * 64
        for v in range(8):
            c = _COS[v]
            for u in range(8):
                acc = 0.0
                for y in range(8):
                    acc += tmp[y * 8 + u] * c[y]
                coef[v * 8 + u] = int(round(acc / q[v * 8 + u]))
    dc = coef[0]
    diff = dc - prev_dc
    cat = _category(diff)
    bits.put(*dc_codes[cat])
    if cat:
        bits.put(diff if diff > 0 else diff + (1 << cat) - 1, cat)
    run = 0
    for i in range(1, 64):
        v = coef[ZIGZAG[i]]
        if v == 0:
            run += 1
            continue
        while run > 15:
            bits.put(*ac_codes[0xF0])
            run -= 16
        cat = _category(v)
        bits.put(*ac_codes[(run << 4) | cat])
        bits.put(v if v > 0 else v + (1 << cat) - 1, cat)
        run = 0
    if run:
        bits.put(*ac_codes[0x00])
    return dc


def _segment(marker, payload):
    return bytes([0xFF, marker]) + (len(payload) + 2).to_bytes(2, "big") + payload


def _dht(cls_id, bits, vals):
    return bytes([cls_id]) + bytes(bits) + bytes(vals)


def encode(rgb, w, h):
    """rgb: bytes/bytearray w*h*3 -> JPEG bytes. w and h multiples of 8."""
    ys, cbs, crs = bytearray(w * h), bytearray(w * h), bytearray(w * h)
    for i in range(w * h):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        ys[i] = max(0, min(255, int(0.299 * r + 0.587 * g + 0.114 * b + 0.5)))
        cbs[i] = max(0, min(255, int(128 - 0.168736 * r - 0.331264 * g + 0.5 * b + 0.5)))
        crs[i] = max(0, min(255, int(128 + 0.5 * r - 0.418688 * g - 0.081312 * b + 0.5)))
    out = bytearray(b"\xff\xd8")
    out += _segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    out += _segment(0xDB, bytes([0]) + bytes(Q_LUM[z] for z in ZIGZAG) + bytes([1]) + bytes(Q_CHR[z] for z in ZIGZAG))
    out += _segment(0xC0, bytes([8]) + h.to_bytes(2, "big") + w.to_bytes(2, "big") + bytes([3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1]))
    out += _segment(0xC4, _dht(0x00, DC_LUM_BITS, DC_VALS) + _dht(0x10, AC_LUM_BITS, AC_LUM_VALS)
                    + _dht(0x01, DC_CHR_BITS, DC_VALS) + _dht(0x11, AC_CHR_BITS, AC_CHR_VALS))
    out += _segment(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]))
    bits = _Bits()
    prev = [0, 0, 0]
    planes = ((ys, Q_LUM, DC_LUM, AC_LUM), (cbs, Q_CHR, DC_CHR, AC_CHR), (crs, Q_CHR, DC_CHR, AC_CHR))
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for ci, (plane, q, dcc, acc) in enumerate(planes):
                block = []
                for y in range(8):
                    o = (by + y) * w + bx
                    block.extend(plane[o:o + 8])
                prev[ci] = _encode_block(bits, block, q, prev[ci], dcc, acc)
    bits.flush()
    out += bits.out
    out += b"\xff\xd9"
    return bytes(out)


# ---- the picture -----------------------------------------------------------------------------
FONT = {
    "A": (0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11), "B": (0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E),
    "C": (0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E), "D": (0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E),
    "E": (0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F), "F": (0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10),
    "G": (0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F), "H": (0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    "I": (0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E), "J": (0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C),
    "K": (0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11), "L": (0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F),
    "M": (0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11), "N": (0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11),
    "O": (0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E), "P": (0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10),
    "Q": (0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D), "R": (0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11),
    "S": (0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E), "T": (0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    "U": (0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E), "V": (0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04),
    "W": (0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A), "X": (0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11),
    "Y": (0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04), "Z": (0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F),
    "0": (0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E), "1": (0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E),
    "2": (0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F), "3": (0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E),
    "4": (0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02), "5": (0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E),
    "6": (0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E), "7": (0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    "8": (0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E), "9": (0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C),
    "-": (0, 0, 0, 0x1F, 0, 0, 0), ".": (0, 0, 0, 0, 0, 0x0C, 0x0C), ":": (0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0),
    "(": (0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02), ")": (0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08),
    "/": (0, 0x01, 0x02, 0x04, 0x08, 0x10, 0), "_": (0, 0, 0, 0, 0, 0, 0x1F), " ": (0, 0, 0, 0, 0, 0, 0),
}
BARS = [(192, 192, 192), (192, 192, 0), (0, 192, 192), (0, 192, 0), (192, 0, 192), (192, 0, 0), (0, 0, 192)]


def _text(img, w, x, y, text, scale, colour):
    for ch in text.upper():
        rows = FONT.get(ch, FONT["-"])
        for ry, bits in enumerate(rows):
            for rx in range(5):
                if bits & (0x10 >> rx):
                    for dy in range(scale):
                        o = ((y + ry * scale + dy) * w + x + rx * scale) * 3
                        for dx in range(scale):
                            img[o + dx * 3:o + dx * 3 + 3] = bytes(colour)
        x += 6 * scale


def test_pattern(name, now=None, w=W, h=H):
    """The frame for second `now`: bars, the name (cut to fit), "EMU CAMERA", the time, a moving block."""
    now = time.time() if now is None else now
    img = bytearray(w * h * 3)
    bar_h = h // 2
    bw = w // len(BARS)
    for y in range(bar_h):
        for i, c in enumerate(BARS):
            x0, x1 = i * bw, (i + 1) * bw if i < len(BARS) - 1 else w
            img[(y * w + x0) * 3:(y * w + x1) * 3] = bytes(c) * (x1 - x0)
    for y in range(bar_h, h):
        img[y * w * 3:(y + 1) * w * 3] = bytes((24, 28, 36)) * w
    sec = int(now)
    bx = (sec % 10) * (w // 10)
    for y in range(bar_h - 16, bar_h):
        img[(y * w + bx) * 3:(y * w + bx + w // 10) * 3] = bytes((255, 255, 255)) * (w // 10)
    label = "".join(c if c.upper() in FONT else "-" for c in (name or "EMU"))
    _text(img, w, 8, bar_h + 10, label[: (w - 16) // 12], 2, (255, 255, 255))
    _text(img, w, 8, bar_h + 40, "EMU CAMERA - TEST PATTERN", 1, (160, 170, 190))
    _text(img, w, 8, bar_h + 60, time.strftime("%H:%M:%S", time.localtime(sec)), 3, (255, 210, 80))
    return encode(img, w, h)

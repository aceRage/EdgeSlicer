"""Render a preview GIF of the EdgeSlicer animated splash.

The real splash draws with wxGraphicsContext inside a live wx app, which cannot be rendered
offscreen from a test without standing up the whole GUI. So this script replicates the same
drawing routine in Pillow. To keep it honest it does NOT hard-code the geometry: it parses the
constants out of src/slic3r/GUI/SplashAnimation.hpp, the same header the C++ animation and the
[Splash] test use, so a change to the shape shows up in the GIF automatically.

The foreground (logo, wordmark, version, loading line) is approximated here -- the real splash
composes it from the existing SVGs through BitmapCache. The GIF is a preview of the ANIMATION;
the foreground is drawn only so the owner can judge the animation behind it at the right weight.
"""

import math
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HPP = os.path.join(REPO, "src", "slic3r", "GUI", "SplashAnimation.hpp")
# Output path: first argument, or splash_preview.gif next to this script.
OUT = (sys.argv[1] if len(sys.argv) > 1
       else os.path.join(os.path.dirname(os.path.abspath(__file__)), "splash_preview.gif"))

FRAMES = 60          # one full 2.5 s loop
SS = 4               # supersampling factor, so hairlines and diagonals are not jagged


def load_constants(path):
    """Pull the static const floats/ints out of the shared header."""
    src = open(path, encoding="utf-8").read()
    consts = {}
    for m in re.finditer(r"static const (?:int|float)\s+(k_\w+)\s*=\s*([0-9.]+)f?;", src):
        consts[m.group(1)] = float(m.group(2))
    required = ["k_loop_ms", "k_design_w", "k_design_h", "k_obj_left",
                "k_obj_right", "k_obj_bottom", "k_obj_top", "k_layer_h", "k_build_end"]
    missing = [k for k in required if k not in consts]
    if missing:
        sys.exit("could not parse %s from %s" % (missing, path))
    return consts


def parse_profile(path):
    """Pull the six numbers that define the silhouette out of the header's half_width_at()."""
    src = open(path, encoding="utf-8").read()
    body = src[src.index("inline float half_width_at"):src.index("// How much of the object")]

    def grab(pattern, count=1):
        m = re.search(pattern, body)
        if m is None:
            sys.exit("could not parse the silhouette profile out of %s" % path)
        return [float(g) for g in m.groups()[:count]]

    hull_end = grab(r"if \(t < ([0-9.]+)f\)[^/]*// Hull")[0]
    deck_end = grab(r"if \(t < ([0-9.]+)f\)[^/]*// Deck")[0]
    keel, flare = grab(r"max_half \* \(([0-9.]+)f \+ ([0-9.]+)f \* std::sqrt", 2)
    cabin, taper = grab(r"max_half \* \(([0-9.]+)f - ([0-9.]+)f \* u\)", 2)
    return hull_end, deck_end, keel, flare, cabin, taper


C = load_constants(HPP)
DESIGN_W = int(C["k_design_w"])
DESIGN_H = int(C["k_design_h"])
OBJ_L, OBJ_R = C["k_obj_left"], C["k_obj_right"]
OBJ_B, OBJ_T = C["k_obj_bottom"], C["k_obj_top"]
LAYER_H = C["k_layer_h"]
BUILD_END = C["k_build_end"]
MAX_HALF = (OBJ_R - OBJ_L) * 0.5
OBJ_H = OBJ_B - OBJ_T
MID_X = (OBJ_L + OBJ_R) * 0.5
LAYERS = max(1, int(OBJ_H / LAYER_H))


# The silhouette's own numbers are parsed out of the header's half_width_at() as well, rather than
# being retyped here, so the preview cannot quietly drift from the shape the slicer actually draws.
PROFILE = parse_profile(HPP)


def half_width_at(t):
    """Mirror of SplashAnim::half_width_at, driven by the numbers parsed from the header."""
    hull_end, deck_end, keel, flare, cabin, taper = PROFILE
    if t < hull_end:
        u = t / hull_end
        return MAX_HALF * (keel + flare * math.sqrt(u))
    if t < deck_end:
        return MAX_HALF
    u = (t - deck_end) / (1.0 - deck_end)
    return MAX_HALF * (cabin - taper * u)


def build_at(phase):
    return min(1.0, max(0.0, phase) / BUILD_END)


def draw_animation(d, phase):
    """Mirror of SplashAnim::draw: the background animation, in design units scaled by SS."""
    def X(v):
        return v * SS
    def Y(v):
        return v * SS

    build = build_at(phase)
    head_y = OBJ_B - OBJ_H * build
    hair = max(1, SS)

    # 1) ghost outline of the whole object
    ghost = []
    for i in range(LAYERS + 1):
        t = i / LAYERS
        ghost.append((X(MID_X - half_width_at(t)), Y(OBJ_B - OBJ_H * t)))
    for i in range(LAYERS, -1, -1):
        t = i / LAYERS
        ghost.append((X(MID_X + half_width_at(t)), Y(OBJ_B - OBJ_H * t)))
    d.line(ghost + [ghost[0]], fill=(0xEF, 0xEF, 0xF1), width=hair, joint="curve")

    # 2) the built part
    if build > 0.001:
        solid = [(X(MID_X - half_width_at(0.0)), Y(OBJ_B))]
        for i in range(LAYERS + 1):
            t = i / LAYERS
            if t > build:
                break
            solid.append((X(MID_X - half_width_at(t)), Y(OBJ_B - OBJ_H * t)))
        solid.append((X(MID_X - half_width_at(build)), Y(head_y)))
        solid.append((X(MID_X + half_width_at(build)), Y(head_y)))
        for i in range(LAYERS, -1, -1):
            t = i / LAYERS
            if t > build:
                continue
            solid.append((X(MID_X + half_width_at(t)), Y(OBJ_B - OBJ_H * t)))
        d.polygon(solid, fill=(0xF4, 0xF4, 0xF6), outline=(0xDF, 0xDF, 0xE3))

        # 3) layer lines
        for i in range(1, LAYERS + 1):
            t = i / LAYERS
            if t > build:
                break
            y = OBJ_B - OBJ_H * t
            hw = half_width_at(t)
            d.line([(X(MID_X - hw), Y(y)), (X(MID_X + hw), Y(y))],
                   fill=(0xE6, 0xE6, 0xEA), width=hair)

    # 4) the slicing line
    if build < 1.0:
        d.line([(X(20.0), Y(head_y)), (X(DESIGN_W - 20.0), Y(head_y))],
               fill=(0xE9, 0xE9, 0xED), width=hair)
        hw = half_width_at(build) + 8.0
        d.line([(X(MID_X - hw), Y(head_y)), (X(MID_X + hw), Y(head_y))],
               fill=(0xCF, 0xCF, 0xD6), width=max(1, int(hair * 1.6)))

    # 5) the plate
    d.line([(X(max(20.0, OBJ_L - 20.0)), Y(OBJ_B + 2.0)), (X(OBJ_R + 20.0), Y(OBJ_B + 2.0))],
           fill=(0xDC, 0xDC, 0xE1), width=max(1, int(hair * 1.4)))


def pick_font(size, bold=False):
    for name in (("segoeuib.ttf", "arialbd.ttf") if bold else ("segoeui.ttf", "arial.ttf")):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_foreground(d):
    """Approximation of the splash foreground, so the animation can be judged behind it.

    The real splash draws splash_app_icon.svg and the EdgeSlicer wordmark through BitmapCache at
    the window's own DPI; this is a stand-in at the same positions and weights.
    """
    def X(v):
        return v * SS
    def Y(v):
        return v * SS

    # Logo icon placeholder: the real one is splash_app_icon.svg, 120x120 at (150, 36).
    d.rounded_rectangle([X(150), Y(36), X(150 + 120), Y(36 + 120)], radius=X(26),
                        outline=(0xD2, 0xD2, 0xD8), width=max(1, SS), fill=(255, 255, 255))
    d.text((X(210), Y(96)), "logo", font=pick_font(int(13 * SS)),
           fill=(0xB6, 0xB6, 0xBE), anchor="mm")

    # Wordmark: "Edge" in ink, "Slicer" in the katana red, centred, at y = 168.
    f_title = pick_font(int(22 * SS), bold=True)
    edge, slicer = "Edge", "Slicer"
    w_edge = d.textlength(edge, font=f_title)
    w_slicer = d.textlength(slicer, font=f_title)
    kern = -1 * SS
    brand_w = w_edge + kern + w_slicer
    bx = (DESIGN_W * SS - brand_w) / 2
    d.text((bx, Y(168)), edge, font=f_title, fill=(23, 23, 23))
    d.text((bx + w_edge + kern, Y(168)), slicer, font=f_title, fill=(0xE0, 0x26, 0x2B))

    # Version line, then the loading line, at the positions the splash uses.
    f_small = pick_font(int(13 * SS))
    d.text((DESIGN_W * SS / 2, Y(168 + 30)), "V2.3.8.1", font=f_small,
           fill=(143, 143, 143), anchor="ma")
    d.text((DESIGN_W * SS / 2, Y(258)), "Loading configuration...", font=f_small,
           fill=(143, 143, 143), anchor="ma")


def render_frame(phase):
    img = Image.new("RGB", (DESIGN_W * SS, DESIGN_H * SS), (255, 255, 255))
    d = ImageDraw.Draw(img)
    draw_animation(d, phase)
    draw_foreground(d)
    # Down-sample to 1x, which is what the splash draws on a 100% display.
    return img.resize((DESIGN_W, DESIGN_H), Image.LANCZOS)


def main():
    frames = [render_frame(i / FRAMES) for i in range(FRAMES)]
    duration = int(round(C["k_loop_ms"] / FRAMES))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    frames[0].save(OUT, save_all=True, append_images=frames[1:], loop=0,
                   duration=duration, optimize=True)
    print("wrote %s" % OUT)
    print("%d frames, %d ms each, loop %d ms, %d layers"
          % (FRAMES, duration, C["k_loop_ms"], LAYERS))
    print("size: %.1f KB" % (os.path.getsize(OUT) / 1024.0))


if __name__ == "__main__":
    main()

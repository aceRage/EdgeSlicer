#!/usr/bin/env python3
"""Bakes the bundled image assets of the Showcase render tab.

Outputs (relative to --out, default resources/textures/showcase/):
  environments/<id>.png        RGBM cube-map atlas, 5 GGX-prefiltered levels (768 x 248, RGBA8)
  environments/environments.json
  floors/<id>.jpg              1024 x 1024 tileable albedo, baseline JPEG q85, sRGB
  floors/floors.json

The script never downloads anything. Fetch the source files listed in ENVIRONMENTS / FLOORS below
by hand (all CC0 1.0; Poly Haven HDRIs as the 2k .hdr, ambientCG materials as the 2K-JPG .zip) and
point the script at them, either one by one or through a cache directory searched recursively for
the file names in the tables:

    python3 scripts/showcase/bake_showcase_assets.py --cache ~/showcase_src
    python3 scripts/showcase/bake_showcase_assets.py --env studio=/x/studio_small_09_2k.hdr ... \
                                                     --floor oak=/x/Wood048_2K-JPG.zip ...
    options: --only envs|floors   --out DIR   --preview DIR (tonemapped sanity images)
             --verify (decode the atlases again, compare against the source, check the SH)

A floor source may be the ambientCG zip (its *_Color.jpg member is read in memory, nothing is
extracted) or any colour image. Needs Python 3.8+, numpy and Pillow.

Conventions (the C++ loader depends on these, do not change them without changing the loader):

* World space is Z-up. Equirect mapping of a source HDRI (the Poly Haven / Blender convention, which
  shows the panorama un-mirrored when seen from inside): for a world direction w = (x, y, z),
      phi = atan2(y, x), theta = asin(z),  u = 0.5 - phi / (2 pi),  v = 0.5 - theta / pi
  with v = 0 the top row (zenith). The image centre (u = 0.5) maps to world +X, u = 0.25 to +Y.
* Cube faces are OpenGL faces +X,-X,+Y,-Y,+Z,-Z in GL cube space. Face texel (i, j), j = 0 the top
  row of the PNG band: s = (i+.5)/N, t = (j+.5)/N, sc = 2s-1, tc = 2t-1, GL direction
      +X ( 1,-tc,-sc)  -X (-1,-tc, sc)  +Y (sc, 1, tc)  -Y (sc,-1,-tc)  +Z (sc,-tc, 1)  -Z (-sc,-tc,-1)
  GL cube space -> world: world = (gl.x, -gl.z, gl.y); world -> GL: gl = (w.x, w.z, -w.y).
* Atlas: level L = 0..4 has face size N_L = 128 >> L, its band starts at y = 0,128,192,224,240 and
  face f sits at x = f * N_L. Unused pixels are (0,0,0,0). Level L is the split-sum GGX prefilter
  (N = V = R) for roughness L/4, alpha = roughness^2; level 0 is the area-averaged radiance.
* RGBM, linear, range 16: m = clamp(max(rgb)/16, 1e-6, 1), a = ceil(255 m)/255,
  stored = clamp(rgb / (16 a), 0, 1); decode rgb = stored * a * 16.
* Each environment is scaled by one factor so that the largest luminance of its diffuse SH term
  over all normals is 1.0 (0.35 for dark_studio). "sh" holds 9 RGB coefficients c_i of the
  Lambertian outgoing radiance for albedo 1, Lout(n) = sum c_i Y_i(n) with n a world normal:
      Y0 = .282095, Y1 = .488603 y, Y2 = .488603 z, Y3 = .488603 x, Y4 = 1.092548 xy,
      Y5 = 1.092548 yz, Y6 = .315392 (3z^2 - 1), Y7 = 1.092548 xz, Y8 = .546274 (x^2 - y^2)
      c_i = L_i * A_l / pi,  A_0 = pi, A_1 = 2pi/3, A_2 = pi/4,  L_i = integral of radiance * Y_i.

Prefilter method: the split-sum integral with N = V = R is
    P(n) = sum_l L(l) D(h) (n.l) / sum_l D(h) (n.l),   h = normalize(n + l),
and with N = V, (n.h)^2 = (1 + n.l) / 2, so the weight depends on n.l only. Instead of
Monte-Carlo importance sampling we evaluate this integral by quadrature over every texel of a
solid-angle-weighted, area-averaged source cube (128 px faces for level 1, 64 for level 2, 32 for
levels 3-4), which is what importance sampling converges to, without noise or fireflies.
"""
import argparse, io, json, math, os, re, sys, zipfile
import numpy as np
from PIL import Image

FETCH_DATE = '2026-10-18'
LICENCE = 'CC0 1.0'
RGBM_RANGE = 16.0
FACE_SIZE = 128
LEVELS = 5
LUMA = np.array([0.2126, 0.7152, 0.0722])

# Source table. URLs as of FETCH_DATE. Credited by site only (CC0 needs no attribution);
# each asset page names its photographers.
ENVIRONMENTS = [
    dict(id='studio', name='Studio softbox', target=1.0, ph='studio_small_09', title='Studio Small 09',
         authors='Poly Haven'),
    dict(id='workshop', name='Workshop', target=1.0, ph='artist_workshop', title='Artist Workshop',
         authors='Poly Haven'),
    dict(id='dark_studio', name='Dark studio', target=0.35, ph='studio_small_02', title='Studio Small 02',
         authors='Poly Haven'),
    dict(id='outdoor', name='Outdoor daylight', target=1.0, ph='kloofendal_48d_partly_cloudy_puresky',
         title='Kloofendal 48d Partly Cloudy (Pure Sky)', authors='Poly Haven'),
    dict(id='overcast', name='Overcast', target=1.0, ph='kloofendal_overcast_puresky',
         title='Kloofendal Overcast (Pure Sky)', authors='Poly Haven'),
    dict(id='sunset', name='Sunset', target=1.0, ph='qwantani_sunset_puresky', title='Qwantani Sunset (Pure Sky)',
         authors='Poly Haven'),
]
for _e in ENVIRONMENTS:
    _e['page'] = 'https://polyhaven.com/a/' + _e['ph']
    _e['download'] = 'https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/%s_2k.hdr' % _e['ph']
    _e['files'] = ['%s_2k.hdr' % _e['ph'], '%s_1k.hdr' % _e['ph'], '%s_4k.hdr' % _e['ph']]

# tile_mm: real-world size of the 1024 px tile. ambientCG gives 80 x 80 cm for Wood048/Wood051 and
# 40 x 40 cm for Fabric066; Concrete034 is 110 x 55 cm and is stacked twice vertically into a square
# (110 x 110 cm). Marble012 and Tiles138 have no published size; the values are estimates.
FLOORS = [
    dict(id='oak', name='Oak', acg='Wood048', tile_mm=800, roughness=0.5),
    dict(id='walnut', name='Walnut', acg='Wood051', tile_mm=800, roughness=0.45),
    dict(id='concrete', name='Concrete', acg='Concrete034', tile_mm=1100, roughness=0.8),
    dict(id='marble', name='Marble', acg='Marble012', tile_mm=600, roughness=0.25),
    dict(id='linen', name='Linen', acg='Fabric066', tile_mm=400, roughness=0.95),
    dict(id='slate', name='Slate tiles', acg='Tiles138', tile_mm=1000, roughness=0.7),
]
for _f in FLOORS:
    _f['page'] = 'https://ambientcg.com/a/' + _f['acg']
    _f['download'] = 'https://ambientcg.com/get?file=%s_2K-JPG.zip' % _f['acg']
    _f['authors'] = 'ambientCG'
    _f['files'] = ['%s_2K-JPG.zip' % _f['acg'], '%s_2K-JPG_Color.jpg' % _f['acg'],
                   '%s_4K-JPG.zip' % _f['acg'], '%s_1K-JPG.zip' % _f['acg']]


# ----------------------------------------------------------------------------------------------
# Radiance RGBE (.hdr) reader
# ----------------------------------------------------------------------------------------------
def read_hdr(path):
    """Returns float32 (H, W, 3) linear radiance, row 0 = top of the image."""
    with open(path, 'rb') as f:
        buf = f.read()
    pos = 0

    def line():
        nonlocal pos
        e = buf.index(b'\n', pos)
        s = buf[pos:e].decode('latin-1').rstrip('\r')
        pos = e + 1
        return s

    magic = line()
    if not magic.startswith('#?'):
        raise ValueError('%s: not a Radiance file' % path)
    exposure = 1.0
    while True:
        s = line()
        if s == '':
            break
        if s.startswith('FORMAT=') and s[7:].strip() != '32-bit_rle_rgbe':
            raise ValueError('%s: unsupported format %s' % (path, s))
        if s.startswith('EXPOSURE='):
            exposure *= float(s[9:])
    res = line().split()
    if len(res) != 4 or res[2] != '+X' or res[0] not in ('-Y', '+Y'):
        raise ValueError('%s: unsupported orientation %r' % (path, res))
    H, W = int(res[1]), int(res[3])
    flip = res[0] == '+Y'
    rgbe = np.zeros((H, W, 4), np.uint8)
    y = 0
    while y < H:
        rle = (8 <= W < 32768 and pos + 4 <= len(buf) and buf[pos] == 2 and buf[pos + 1] == 2
               and buf[pos + 2] < 128 and (buf[pos + 2] << 8 | buf[pos + 3]) == W)
        if not rle:
            break  # flat / old-style data from here on, as in Ward's rgbe.c
        pos += 4
        row = bytearray()
        for ch in range(4):
            end = (ch + 1) * W
            while len(row) < end:
                c = buf[pos]
                pos += 1
                if c > 128:
                    n = c - 128
                    if len(row) + n > end:
                        raise ValueError('%s: bad RLE run' % path)
                    row += bytes((buf[pos],)) * n
                    pos += 1
                else:
                    if c == 0 or len(row) + c > end:
                        raise ValueError('%s: bad RLE dump' % path)
                    row += buf[pos:pos + c]
                    pos += c
        rgbe[y] = np.frombuffer(bytes(row), np.uint8).reshape(4, W).T
        y += 1
    if y < H:  # flat pixels with optional old-style (1,1,1,n) repeat runs
        flat = rgbe.reshape(-1, 4)
        i, shift = y * W, 0
        while i < H * W:
            px = buf[pos:pos + 4]
            if len(px) < 4:
                raise ValueError('%s: truncated' % path)
            pos += 4
            if px[0] == 1 and px[1] == 1 and px[2] == 1:
                n = px[3] << shift
                if i == 0 or i + n > H * W:
                    raise ValueError('%s: bad old-style run' % path)
                flat[i:i + n] = flat[i - 1]
                i += n
                shift += 8
            else:
                flat[i] = np.frombuffer(px, np.uint8)
                i += 1
                shift = 0
    e = rgbe[..., 3].astype(np.int32)
    scale = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0)
    img = (rgbe[..., :3].astype(np.float64) * scale[..., None]) / exposure
    if flip:
        img = img[::-1]
    return img.astype(np.float32)


# ----------------------------------------------------------------------------------------------
# Geometry helpers
# ----------------------------------------------------------------------------------------------
def gl_to_world(g):
    return np.stack([g[..., 0], -g[..., 2], g[..., 1]], axis=-1)


def world_to_gl(w):
    return np.stack([w[..., 0], w[..., 2], -w[..., 1]], axis=-1)


def equirect_dirs(H, W, du=0.5, dv=0.5):
    """World directions of the equirect sample points (row, col) + (dv, du); shape (H, W, 3)."""
    u = (np.arange(W) + du) / W
    v = (np.arange(H) + dv) / H
    phi = (0.5 - u) * 2 * np.pi
    theta = (0.5 - v) * np.pi
    ct, st = np.cos(theta)[:, None], np.sin(theta)[:, None]
    return np.stack([ct * np.cos(phi)[None, :], ct * np.sin(phi)[None, :], np.broadcast_to(st, (H, W))], axis=-1)


def equirect_solid_angles(H, W):
    """Exact solid angle of every pixel row, shape (H, 1)."""
    edges = np.pi / 2 - np.arange(H + 1) * np.pi / H
    return ((np.sin(edges[:-1]) - np.sin(edges[1:])) * (2 * np.pi / W))[:, None]


def dir_to_equirect_uv(w):
    phi = np.arctan2(w[..., 1], w[..., 0])
    theta = np.arcsin(np.clip(w[..., 2], -1, 1))
    return 0.5 - phi / (2 * np.pi), 0.5 - theta / np.pi


def sample_equirect(img, w):
    """Bilinear lookup (wrapping in u) of world directions w (..., 3)."""
    H, W, _ = img.shape
    u, v = dir_to_equirect_uv(w)
    x = u * W - 0.5
    y = np.clip(v * H - 0.5, 0, H - 1)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    y1 = np.minimum(y0 + 1, H - 1)
    x0w, x1w = x0 % W, (x0 + 1) % W
    return ((img[y0, x0w] * (1 - fx) + img[y0, x1w] * fx) * (1 - fy)
            + (img[y1, x0w] * (1 - fx) + img[y1, x1w] * fx) * fy)


def cube_face_dirs(N):
    """GL-space unit directions of all texel centres, shape (6, N, N, 3) indexed [face, j, i]."""
    s = (np.arange(N) + 0.5) / N
    sc = (2 * s - 1)[None, :].repeat(N, 0)   # varies with i (column)
    tc = (2 * s - 1)[:, None].repeat(N, 1)   # varies with j (row)
    one = np.ones_like(sc)
    faces = [
        (one, -tc, -sc),     # +X
        (-one, -tc, sc),     # -X
        (sc, one, tc),       # +Y
        (sc, -one, -tc),     # -Y
        (sc, -tc, one),      # +Z
        (-sc, -tc, -one),    # -Z
    ]
    d = np.stack([np.stack(f, axis=-1) for f in faces])
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def cube_texel_solid_angles(N):
    """Solid angle of every texel of a face (same for all faces), shape (N, N)."""
    e = np.linspace(-1, 1, N + 1)
    a = lambda x, y: np.arctan2(x * y, np.sqrt(x * x + y * y + 1))
    X0, Y0 = np.meshgrid(e[:-1], e[:-1], indexing='xy')
    X1, Y1 = np.meshgrid(e[1:], e[1:], indexing='xy')
    return a(X0, Y0) - a(X0, Y1) - a(X1, Y0) + a(X1, Y1)


def gl_dir_to_face_texel(g, N):
    """Inverse of cube_face_dirs (standard GL face selection): returns face, i (column), j (row)."""
    x, y, z = g[..., 0], g[..., 1], g[..., 2]
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    face = np.where((ax >= ay) & (ax >= az), np.where(x >= 0, 0, 1),
                    np.where(ay >= az, np.where(y >= 0, 2, 3), np.where(z >= 0, 4, 5)))
    ma = np.choose(face, [ax, ax, ay, ay, az, az])
    sc = np.choose(face, [-z, z, x, x, x, -x]) / ma
    tc = np.choose(face, [-y, -y, z, -z, -y, -y]) / ma
    i = np.clip(np.floor((sc + 1) * 0.5 * N), 0, N - 1).astype(np.int64)
    j = np.clip(np.floor((tc + 1) * 0.5 * N), 0, N - 1).astype(np.int64)
    return face, i, j


# ----------------------------------------------------------------------------------------------
# Environment processing
# ----------------------------------------------------------------------------------------------
def equirect_to_cube(img, N, sub=2):
    """Area-averaged cube map: every source pixel (split into sub x sub pieces) is accumulated into
    the texel its direction falls in, weighted by its solid angle. Returns (6, N, N, 3)."""
    H, W, _ = img.shape
    dw = np.broadcast_to(equirect_solid_angles(H, W), (H, W)).reshape(-1) / (sub * sub)
    rad = img.reshape(-1, 3).astype(np.float64)
    acc = np.zeros((6 * N * N, 3))
    wsum = np.zeros(6 * N * N)
    for a in range(sub):
        for b in range(sub):
            w = equirect_dirs(H, W, (a + 0.5) / sub, (b + 0.5) / sub).reshape(-1, 3)
            f, i, j = gl_dir_to_face_texel(world_to_gl(w), N)
            idx = (f * N + j) * N + i
            wsum += np.bincount(idx, weights=dw, minlength=6 * N * N)
            for c in range(3):
                acc[:, c] += np.bincount(idx, weights=rad[:, c] * dw, minlength=6 * N * N)
    if (wsum <= 0).any():
        raise RuntimeError('cube texel without source samples; source resolution too low')
    return (acc / wsum[:, None]).reshape(6, N, N, 3)


def cube_downsample(cube):
    """2x2 solid-angle-weighted box filter of a (6, N, N, 3) cube."""
    N = cube.shape[1]
    dw = cube_texel_solid_angles(N)[None, :, :, None]
    num = (cube * dw).reshape(6, N // 2, 2, N // 2, 2, 3).sum(axis=(2, 4))
    den = dw.reshape(1, N // 2, 2, N // 2, 2, 1).sum(axis=(2, 4))
    return num / den


def ggx_prefilter(src, n_out, roughness, chunk=256):
    """Split-sum GGX prefilter (N = V = R) by quadrature over every texel of the source cube."""
    alpha = roughness * roughness
    a2 = alpha * alpha
    Ns = src.shape[1]
    L = cube_face_dirs(Ns).reshape(-1, 3)
    dw = np.broadcast_to(cube_texel_solid_angles(Ns)[None], (6, Ns, Ns)).reshape(-1)
    rhs = np.concatenate([src.reshape(-1, 3) * dw[:, None], dw[:, None]], axis=1)  # (S, 4)
    Nd = cube_face_dirs(n_out).reshape(-1, 3)
    out = np.empty((Nd.shape[0], 3))
    for k in range(0, Nd.shape[0], chunk):
        c = Nd[k:k + chunk] @ L.T                        # n.l
        nh2 = 0.5 * (1.0 + c)                            # (n.h)^2 with n = v
        D = a2 / (np.pi * (nh2 * (a2 - 1.0) + 1.0) ** 2)
        wgt = D * np.maximum(c, 0.0)
        r = wgt @ rhs
        out[k:k + chunk] = r[:, :3] / r[:, 3:4]
    return out.reshape(6, n_out, n_out, 3)


SH_K = np.array([0.282095, 0.488603, 0.488603, 0.488603, 1.092548, 1.092548, 0.315392, 1.092548, 0.546274])
SH_A = np.array([np.pi, 2 * np.pi / 3, 2 * np.pi / 3, 2 * np.pi / 3] + [np.pi / 4] * 5)


def sh_basis(w):
    x, y, z = w[..., 0], w[..., 1], w[..., 2]
    one = np.ones_like(x)
    return np.stack([one, y, z, x, x * y, y * z, 3 * z * z - 1, x * z, x * x - y * y], axis=-1) * SH_K


def diffuse_sh(img):
    """9 x 3 coefficients c_i = L_i A_l / pi of the Lambertian outgoing radiance (albedo 1)."""
    H, W, _ = img.shape
    dw = equirect_solid_angles(H, W)
    w = equirect_dirs(H, W)
    coeffs = np.zeros((9, 3))
    for r0 in range(0, H, 128):
        Y = sh_basis(w[r0:r0 + 128])                                     # (h, W, 9)
        coeffs += np.einsum('hwk,hwc->kc', Y * dw[r0:r0 + 128, :, None], img[r0:r0 + 128].astype(np.float64))
    return coeffs * (SH_A / np.pi)[:, None]


def sh_eval(coeffs, n):
    return sh_basis(n) @ coeffs


def fibonacci_sphere(n):
    k = np.arange(n) + 0.5
    z = 1 - 2 * k / n
    r = np.sqrt(1 - z * z)
    phi = k * np.pi * (3 - np.sqrt(5))
    return np.stack([r * np.cos(phi), r * np.sin(phi), z], axis=-1)


def direct_lambert(img, normals):
    """Brute-force (1/pi) * integral of L(w) max(n.w, 0) dw over the source equirect."""
    H, W, _ = img.shape
    dw = equirect_solid_angles(H, W)
    w = equirect_dirs(H, W)
    rad = (img.astype(np.float64) * dw[..., None]).reshape(-1, 3)
    wf = w.reshape(-1, 3)
    return np.stack([np.maximum(wf @ n, 0) @ rad for n in normals]) / np.pi


def dominant_light(cube128, cone_deg=12.0, min_fraction=0.25):
    """World direction of the dominant light (sun / brightest softbox) or None when there is no
    clear one. A 12 degree cone around the best direction must deliver at least min_fraction of the
    cosine-weighted irradiance arriving at that direction."""
    c32 = cube_downsample(cube_downsample(cube128))
    N = c32.shape[1]
    d = cube_face_dirs(N).reshape(-1, 3)
    p = (c32.reshape(-1, 3) @ LUMA) * np.broadcast_to(cube_texel_solid_angles(N)[None], (6, N, N)).reshape(-1)
    cosc = math.cos(math.radians(cone_deg))
    best, best_j = -1.0, 0
    for k in range(0, d.shape[0], 1024):
        s = ((d[k:k + 1024] @ d.T) > cosc) @ p
        j = int(np.argmax(s))
        if s[j] > best:
            best, best_j = s[j], k + j
    m = (d @ d[best_j]) > cosc
    g = (p[m, None] * d[m]).sum(0)
    g /= np.linalg.norm(g)
    # refine on the 128 cube: centroid of the bright texels inside the cone
    d1 = cube_face_dirs(cube128.shape[1]).reshape(-1, 3)
    y1 = cube128.reshape(-1, 3) @ LUMA
    dw1 = np.broadcast_to(cube_texel_solid_angles(cube128.shape[1])[None], cube128.shape[:3]).reshape(-1)
    m1 = (d1 @ g) > cosc
    thr = 0.5 * y1[m1].max()
    m2 = m1 & (y1 >= thr)
    g2 = ((y1 * dw1)[m2, None] * d1[m2]).sum(0)
    g2 /= np.linalg.norm(g2)
    e_cone = (p * np.maximum(d @ g2, 0) * ((d @ g2) > cosc)).sum()
    e_all = (p * np.maximum(d @ g2, 0)).sum()
    frac = e_cone / e_all
    return gl_to_world(g2), frac, frac >= min_fraction


def rgbm_encode(rgb):
    m = np.clip(rgb.max(axis=-1) / RGBM_RANGE, 1e-6, 1.0)
    a = np.ceil(m * 255.0) / 255.0
    stored = np.clip(rgb / (a[..., None] * RGBM_RANGE), 0.0, 1.0)
    out = np.empty(rgb.shape[:-1] + (4,), np.uint8)
    out[..., :3] = np.round(stored * 255.0).astype(np.uint8)
    out[..., 3] = np.round(a * 255.0).astype(np.uint8)
    return out


def rgbm_decode(px):
    px = px.astype(np.float64) / 255.0
    return px[..., :3] * px[..., 3:4] * RGBM_RANGE


def level_origin(L):
    return sum(FACE_SIZE >> k for k in range(L))


def build_atlas(levels):
    H = sum(FACE_SIZE >> k for k in range(LEVELS))
    atlas = np.zeros((H, 6 * FACE_SIZE, 4), np.uint8)
    for L, cube in enumerate(levels):
        N = FACE_SIZE >> L
        y0 = level_origin(L)
        enc = rgbm_encode(cube)
        for f in range(6):
            atlas[y0:y0 + N, f * N:(f + 1) * N] = enc[f]
    return atlas


def atlas_levels(atlas):
    out = []
    for L in range(LEVELS):
        N = FACE_SIZE >> L
        y0 = level_origin(L)
        band = atlas[y0:y0 + N, :6 * N]
        out.append(np.stack([rgbm_decode(band[:, f * N:(f + 1) * N]) for f in range(6)]))
    return out


def sig5(v):
    return float('%.5g' % v)


def bake_environment(env, path, out_dir, verify, preview_dir):
    print('[env] %s <- %s' % (env['id'], path))
    img = read_hdr(path).astype(np.float64)
    H, W, _ = img.shape
    print('      %dx%d, max %.1f, mean %.4f' % (W, H, img.max(), img.mean()))
    sh = diffuse_sh(img)
    probe = fibonacci_sphere(20000)
    peak = (sh_eval(sh, probe) @ LUMA).max()
    k = env['target'] / peak
    img *= k
    sh *= k
    report = dict(id=env['id'], scale=k)

    base = equirect_to_cube(img, FACE_SIZE)
    levels = [base]
    c64 = cube_downsample(base)
    c32 = cube_downsample(c64)
    for L in range(1, LEVELS):
        src = {1: base, 2: c64}.get(L, c32)
        levels.append(ggx_prefilter(src, FACE_SIZE >> L, L / (LEVELS - 1)))
    atlas = build_atlas(levels)
    os.makedirs(out_dir, exist_ok=True)
    Image.fromarray(atlas, 'RGBA').save(os.path.join(out_dir, env['id'] + '.png'), optimize=True)

    sun, frac, clear = dominant_light(base)
    report['sun_fraction'] = frac
    entry = dict(id=env['id'], name=env['name'], file=env['id'] + '.png',
                 sh=[[sig5(v) for v in row] for row in sh])
    if clear:
        sun = sun / np.linalg.norm(sun)
        entry['sun_dir'] = [round(float(v), 5) for v in sun]
    entry.update(source=env['page'], authors=env['authors'], licence=LICENCE)
    print('      scale %.5g, dominant light %s (cone fraction %.2f%s)' % (
        k, np.round(sun, 3).tolist(), frac, '' if clear else ', omitted'))

    if verify:
        rng = np.random.default_rng(1)
        nr = rng.normal(size=(20, 3))
        nr /= np.linalg.norm(nr, axis=1, keepdims=True)
        ref = direct_lambert(img, nr) @ LUMA
        approx = sh_eval(sh, nr) @ LUMA
        rel = np.abs(approx - ref) / np.maximum(ref, 1e-9)
        report['sh_max_rel_err'] = float(rel.max())
        report['sh_max_err_vs_peak'] = float((np.abs(approx - ref) / env['target']).max())
        dec = atlas_levels(np.asarray(Image.open(os.path.join(out_dir, env['id'] + '.png'))))
        sizes = [d.shape[1] for d in dec]
        assert sizes == [128, 64, 32, 16, 8], sizes
        # level 0 against the source, looked up through the forward face table (independent of the
        # face selection used for baking); compare tonemapped values to stay robust to the clamp
        g = cube_face_dirs(FACE_SIZE)
        srcv = sample_equirect(img, gl_to_world(g))
        tm = lambda x: x / (1 + x)
        report['l0_mean_abs_tm_err'] = float(np.abs(tm(dec[0]) - tm(np.minimum(srcv, RGBM_RANGE))).mean())
        report['l0_mean_tm'] = float(tm(dec[0]).mean())
        # decoded vs float (RGBM quantisation), relative, values below the clamp
        errs = []
        for d, f in zip(dec, levels):
            ok = f.max(-1) < RGBM_RANGE * 0.99
            errs.append(float((np.abs(d - f).max(-1)[ok] / np.maximum(f.max(-1)[ok], 1e-3)).mean()))
        report['rgbm_mean_rel_err'] = errs
        report['level_sizes'] = sizes
        print('      verify: SH max rel err %.3f (max err / peak %.3f), L0 vs source tonemapped err %.4f,'
              ' RGBM rel err %s' % (report['sh_max_rel_err'], report['sh_max_err_vs_peak'],
                                   report['l0_mean_abs_tm_err'], ['%.4f' % e for e in errs]))
    if preview_dir:
        write_env_preview(env['id'], img, os.path.join(out_dir, env['id'] + '.png'), preview_dir)
    return entry, report


def tonemap8(x, exposure=1.0):
    y = x * exposure
    y = y / (1.0 + y)
    y = np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(np.maximum(y, 0), 1 / 2.4) - 0.055)
    return (np.clip(y, 0, 1) * 255 + 0.5).astype(np.uint8)


def write_env_preview(env_id, img, atlas_path, preview_dir):
    """Cross layout of every decoded level (world +Z = GL +Y face on top) plus an equirect rebuilt
    from the decoded level 0 next to the source equirect."""
    os.makedirs(preview_dir, exist_ok=True)
    dec = atlas_levels(np.asarray(Image.open(atlas_path)))
    tiles = []
    for L, cube in enumerate(dec):
        N = cube.shape[1]
        s = 128 // N
        cross = np.zeros((3 * N, 4 * N, 3))
        # GL cross: +Y top, -X +Z +X -Z middle row, -Y bottom
        cross[0:N, N:2 * N] = cube[2]
        cross[N:2 * N, 0:N] = cube[1]
        cross[N:2 * N, N:2 * N] = cube[4]
        cross[N:2 * N, 2 * N:3 * N] = cube[0]
        cross[N:2 * N, 3 * N:4 * N] = cube[5]
        cross[2 * N:3 * N, N:2 * N] = cube[3]
        tiles.append(np.kron(tonemap8(cross), np.ones((s, s, 1), np.uint8)))
    Image.fromarray(np.concatenate(tiles[:3], axis=1)).save(os.path.join(preview_dir, env_id + '_cross_L012.png'))
    Image.fromarray(np.concatenate(tiles[3:], axis=1)).save(os.path.join(preview_dir, env_id + '_cross_L34.png'))
    # equirect rebuilt from decoded level 0 (nearest texel) vs the source
    He, We = 256, 512
    w = equirect_dirs(He, We)
    f, i, j = gl_dir_to_face_texel(world_to_gl(w), FACE_SIZE)
    rebuilt = dec[0][f, j, i]
    srcs = sample_equirect(img, w)
    Image.fromarray(np.concatenate([tonemap8(srcs), tonemap8(rebuilt)], axis=0)).save(
        os.path.join(preview_dir, env_id + '_equirect_src_vs_atlas.png'))


# ----------------------------------------------------------------------------------------------
# Floors
# ----------------------------------------------------------------------------------------------
def load_colour_image(path):
    if path.lower().endswith('.zip'):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.endswith('_Color.jpg') and '/' not in n]
            if len(names) != 1:
                raise ValueError('%s: expected exactly one *_Color.jpg member, found %r' % (path, names))
            data = z.read(names[0])
        return Image.open(io.BytesIO(data)).convert('RGB'), names[0]
    return Image.open(path).convert('RGB'), os.path.basename(path)


def srgb_to_linear(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(x):
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.maximum(x, 0), 1 / 2.4) - 0.055)


def resize_tileable(im, size):
    """Lanczos resize in linear light with wrap-around padding, so the result still tiles."""
    a = srgb_to_linear(np.asarray(im).astype(np.float64) / 255.0)
    S = a.shape[0]
    scale = size / S
    pad_out = 8
    pad_src = int(round(pad_out / scale))
    if abs(pad_src * scale - pad_out) > 1e-9:
        pad_src, pad_out = 0, 0
    if pad_src:
        a = np.pad(a, ((pad_src, pad_src), (pad_src, pad_src), (0, 0)), mode='wrap')
    big = size + 2 * pad_out
    chans = []
    for c in range(3):
        ch = Image.fromarray(a[..., c].astype(np.float32), 'F').resize((big, big), Image.LANCZOS)
        chans.append(np.asarray(ch))
    r = np.stack(chans, axis=-1)[pad_out:pad_out + size, pad_out:pad_out + size]
    r = linear_to_srgb(np.clip(r, 0, 1))
    return Image.fromarray((r * 255 + 0.5).astype(np.uint8), 'RGB')


def bake_floor(fl, path, out_dir):
    im, member = load_colour_image(path)
    w, h = im.size
    if w == 2 * h:      # e.g. 110 x 55 cm: stack two copies vertically into a square
        sq = Image.new('RGB', (w, w)); sq.paste(im, (0, 0)); sq.paste(im, (0, h)); im = sq
    elif h == 2 * w:
        sq = Image.new('RGB', (h, h)); sq.paste(im, (0, 0)); sq.paste(im, (w, 0)); im = sq
    elif w != h:
        raise ValueError('%s: non-square source %dx%d' % (path, w, h))
    out = resize_tileable(im, 1024)
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, fl['id'] + '.jpg')
    out.save(dst, 'JPEG', quality=85, optimize=True, progressive=False)
    size = os.path.getsize(dst)
    print('[floor] %s <- %s (%s, %dx%d) -> %d KB' % (fl['id'], path, member, w, h, size // 1024))
    return dict(id=fl['id'], name=fl['name'], file=fl['id'] + '.jpg', tile_mm=fl['tile_mm'],
                roughness=fl['roughness'], source=fl['page'], authors=fl['authors'], licence=LICENCE)


# ----------------------------------------------------------------------------------------------
def write_json(path, obj):
    """Indented JSON with every list of plain numbers kept on one line."""
    text = json.dumps(obj, indent=2)
    text = re.sub(r'\[\s+([-+0-9.eE,\s]+?)\s+\]',
                  lambda m: '[' + ', '.join(v.strip() for v in m.group(1).split(',')) + ']', text)
    json.loads(text)
    with open(path, 'w', newline='\n') as f:
        f.write(text + '\n')


def find_in_cache(cache, names):
    if not cache:
        return None
    for n in names:  # names are in order of preference
        for root, _, files in sorted(os.walk(cache)):
            if n in files:
                return os.path.join(root, n)
    return None


def parse_pairs(pairs, valid):
    out = {}
    for p in pairs or []:
        k, _, v = p.partition('=')
        if k not in valid or not v:
            raise SystemExit('bad argument %r, expected one of %s=PATH' % (p, '|'.join(valid)))
        out[k] = v
    return out


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cache', help='directory searched recursively for the source files')
    ap.add_argument('--env', action='append', help='ID=PATH of a source .hdr')
    ap.add_argument('--floor', action='append', help='ID=PATH of a source zip / colour image')
    ap.add_argument('--out', default=os.path.join(repo, 'resources', 'textures', 'showcase'))
    ap.add_argument('--only', choices=['envs', 'floors'])
    ap.add_argument('--verify', action='store_true')
    ap.add_argument('--preview', help='write tonemapped preview images to this directory')
    args = ap.parse_args()

    if args.only != 'floors':
        given = parse_pairs(args.env, [e['id'] for e in ENVIRONMENTS])
        paths = {e['id']: given.get(e['id']) or find_in_cache(args.cache, e['files']) for e in ENVIRONMENTS}
        missing = [e['download'] for e in ENVIRONMENTS if not paths[e['id']]]
        if missing:
            raise SystemExit('missing environment sources, download:\n  ' + '\n  '.join(missing))
        out_dir = os.path.join(args.out, 'environments')
        entries, reports = [], []
        for e in ENVIRONMENTS:
            entry, rep = bake_environment(e, paths[e['id']], out_dir, args.verify, args.preview)
            entries.append(entry)
            reports.append(rep)
        manifest = dict(version=1, rgbm_range=int(RGBM_RANGE), face_size=FACE_SIZE, levels=LEVELS,
                        environments=entries)
        write_json(os.path.join(out_dir, 'environments.json'), manifest)
        if args.verify:
            print(json.dumps(reports, indent=1))

    if args.only != 'envs':
        given = parse_pairs(args.floor, [f['id'] for f in FLOORS])
        paths = {f['id']: given.get(f['id']) or find_in_cache(args.cache, f['files']) for f in FLOORS}
        missing = [f['download'] for f in FLOORS if not paths[f['id']]]
        if missing:
            raise SystemExit('missing floor sources, download:\n  ' + '\n  '.join(missing))
        out_dir = os.path.join(args.out, 'floors')
        entries = [bake_floor(f, paths[f['id']], out_dir) for f in FLOORS]
        write_json(os.path.join(out_dir, 'floors.json'), dict(version=1, floors=entries))


if __name__ == '__main__':
    main()

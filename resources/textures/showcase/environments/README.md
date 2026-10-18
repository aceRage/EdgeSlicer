# Showcase environment maps

Image-based lighting for the Showcase render tab. Each `<id>.png` is a prefiltered cube-map atlas
baked from a Poly Haven HDRI by `scripts/showcase/bake_showcase_assets.py`. `environments.json`
lists them together with their diffuse SH lighting and dominant light direction.

All sources are CC0 1.0 Universal (public domain dedication,
https://creativecommons.org/publicdomain/zero/1.0/). Attribution is not required; each source page
below credits the photographers. Sources were fetched on 2026-10-18.

| File | Source asset | Credit | Licence | Original file used |
|---|---|---|---|---|
| `studio.png` | Studio Small 09 - https://polyhaven.com/a/studio_small_09 | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/studio_small_09_2k.hdr |
| `workshop.png` | Artist Workshop - https://polyhaven.com/a/artist_workshop | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/artist_workshop_2k.hdr |
| `dark_studio.png` | Studio Small 02 - https://polyhaven.com/a/studio_small_02 | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/studio_small_02_2k.hdr |
| `outdoor.png` | Kloofendal 48d Partly Cloudy (Pure Sky) - https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr |
| `overcast.png` | Kloofendal Overcast (Pure Sky) - https://polyhaven.com/a/kloofendal_overcast_puresky | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_overcast_puresky_2k.hdr |
| `sunset.png` | Qwantani Sunset (Pure Sky) - https://polyhaven.com/a/qwantani_sunset_puresky | Poly Haven | CC0 1.0 | https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/qwantani_sunset_puresky_2k.hdr |

`environments.json` is generated from the same sources (SH coefficients, dominant light direction).

## Processing

For every HDRI (2048 x 1024 Radiance .hdr):

1. Decoded with the script's own RGBE reader (new-style RLE scanlines, flat/old-RLE fallback).
2. Scaled by one factor so that the largest luminance (Rec. 709 weights) of the diffuse SH term
   below, over all normal directions, is 1.0 (0.35 for `dark_studio`).
3. Level 0: the source is resampled to a 128 px cube by area averaging (every source pixel, split
   into 2 x 2 pieces, is accumulated into the texel its direction falls into, weighted by its solid
   angle), so there is no point-sampling aliasing.
4. Levels 1-4: split-sum GGX prefilter (N = V = R, weight D(h) * N.L) for roughness L/4,
   alpha = roughness^2, evaluated by quadrature over every texel of an area-averaged source cube
   (128 px for level 1, 64 px for level 2, 32 px for levels 3-4). This is the integral GGX
   importance sampling estimates, computed without noise or fireflies.
5. RGBM encoded and written as an 8-bit RGBA PNG.
6. 9 diffuse SH coefficients and the dominant light direction (sun or brightest softbox) are
   computed from the scaled source. `sun_dir` is omitted when a 12 degree cone around the
   brightest direction delivers less than 25% of the irradiance arriving from that direction
   (`overcast`).

## Format

World space is Z-up. The equirect sources are mapped the Poly Haven / Blender way (zenith at the top
row, image centre towards world +X, panorama not mirrored when seen from inside).

Atlas, 768 x 248 RGBA8:

- Level L = 0..4 has face size N = 128 >> L (128, 64, 32, 16, 8). Its band starts at
  y = 0, 128, 192, 224, 240; face f sits at x = f * N in the order +X, -X, +Y, -Y, +Z, -Z of the
  OpenGL cube. Unused pixels are (0, 0, 0, 0).
- Face texel (i, j), j = 0 the top row of the band: s = (i+0.5)/N, t = (j+0.5)/N, sc = 2s-1,
  tc = 2t-1. GL direction: +X (1,-tc,-sc), -X (-1,-tc,sc), +Y (sc,1,tc), -Y (sc,-1,-tc),
  +Z (sc,-tc,1), -Z (-sc,-tc,-1). GL cube space to world: world = (gl.x, -gl.z, gl.y), so world up
  (+Z) is the GL +Y face.
- Level L is the GGX-prefiltered radiance for roughness L/4; level 0 is the radiance itself.
- RGBM, linear, range 16: decode rgb = stored_rgb * alpha * 16 (bytes / 255). Values above 16 are
  clamped (the sun discs).

`environments.json`: `rgbm_range` 16, `face_size` 128, `levels` 5. Per environment, `sh` holds 9 RGB
coefficients c_i of the Lambertian outgoing radiance for albedo 1 (irradiance / pi) as a function of
the world normal n = (x, y, z), Lout(n) = sum c_i Y_i(n), with Y0 = 0.282095, Y1 = 0.488603 y,
Y2 = 0.488603 z, Y3 = 0.488603 x, Y4 = 1.092548 xy, Y5 = 1.092548 yz, Y6 = 0.315392 (3z^2 - 1),
Y7 = 1.092548 xz, Y8 = 0.546274 (x^2 - y^2); c_i = L_i * A_l / pi with A_0 = pi, A_1 = 2pi/3,
A_2 = pi/4 and L_i the projection of the radiance onto Y_i. Numbers have 5 significant digits.
`sun_dir` is a unit world direction.

Regenerate with `python3 scripts/showcase/bake_showcase_assets.py --cache <dir with the .hdr files>`.

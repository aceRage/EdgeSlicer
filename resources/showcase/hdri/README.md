# Showcase HDRI environments

Sharp 2k equirectangular panoramas (Radiance `.hdr`) that light the model and fill the backdrop of
the Showcase tab. `hdri.json` lists them; `src/libslic3r/Showcase/ShowcaseHdr.*` loads and bakes
them. All five are Poly Haven assets under CC0 1.0 (public domain, attribution not required;
https://polyhaven.com/license), the same sources `resources/textures/showcase/environments/README.md`
lists for the baked 128 px environments. Owner approved bundling them on 2026-11-01 (design D-R2);
the files are our own copies, never fetched from the Poly Haven API (D-R3).

| Id | File | Source page | Licence | Bytes | SHA-256 |
|---|---|---|---|---|---|
| `hdri_studio` | `studio_small_09_2k.hdr` | https://polyhaven.com/a/studio_small_09 | CC0 1.0 | 6,312,947 | `36724313c0fc66dab126ff90f081b85a0b1ef47be65eda1417bd20b033f0573f` |
| `hdri_outdoor` (has a sun) | `kloofendal_48d_partly_cloudy_puresky_2k.hdr` | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | CC0 1.0 | 5,451,493 | `5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198` |
| `hdri_workshop` (window light) | `artist_workshop_2k.hdr` | https://polyhaven.com/a/artist_workshop | CC0 1.0 | 6,756,758 | `09e871a2f2bb34395a6c21c9bea78b0e5d9cd3abff4d6da47c547da373342159` |
| `hdri_loft` | `photo_studio_loft_hall_2k.hdr` | https://polyhaven.com/a/photo_studio_loft_hall | CC0 1.0 | 6,432,146 | `2eadabfce70a1f27d958aa18b2572d75883f9964f4656c10e706a44cafe129d0` |
| `hdri_sunset` (golden hour) | `venice_sunset_2k.hdr` | https://polyhaven.com/a/venice_sunset | CC0 1.0 | 5,723,224 | `cbfac020ee17ab36016ccc7fbe9020b828587b9864af6ecda0a93089914889a6` |

About 30.7 MB in all (five files). `.gitattributes` marks `*.hdr` binary.

A file entry in `hdri.json` has `id`, `name`, `file`, `source`, `authors`, `licence`. An entry may
instead have `"procedural": "sky_sun" | "studio" | "interior"` (a panorama generated in code); the
unit tests use those generators and none is listed in the shipped manifest. If a listed file is missing
or unreadable the renderer logs a warning and the scene falls back to its backdrop colours.

Placement defaults (1h-2, optional): `"ground": { "height_mm", "radius_mm" }` is the shot's camera
height and room radius; only their ratio is used, for the automatic dome radius of "Stand on the
environment's floor" (the automatic camera height comes from the model's size). `"backdrop_pitch"`
(degrees, + = more sky) is what the panel sets the backdrop tilt to when the HDRI is chosen. The
values are estimates from looking at the panoramas, not measurements.

## Catalogue for phase 1h-3 (in-app downloads; not in this folder, no download code yet)

The owner wants these Poly Haven CC0 2k panoramas as download options (2026-11-01), served from our
own mirror (the edgeslicer-deps release assets; D-R3), never from the Poly Haven API. When 1h-3 lands
they get manifest entries with `"downloadable": true`, a `download_url` and a `sha256`:

| Poly Haven id | Source page |
|---|---|
| `brown_photostudio_02` | https://polyhaven.com/a/brown_photostudio_02 |
| `blue_photo_studio` | https://polyhaven.com/a/blue_photo_studio |
| `industrial_sunset_puresky` | https://polyhaven.com/a/industrial_sunset_puresky |
| `kiara_1_dawn` | https://polyhaven.com/a/kiara_1_dawn |
| `empty_warehouse_01` | https://polyhaven.com/a/empty_warehouse_01 |
| `abandoned_workshop_02` | https://polyhaven.com/a/abandoned_workshop_02 |

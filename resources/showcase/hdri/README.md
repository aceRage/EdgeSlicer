# Showcase HDRI environments

Sharp 2k equirectangular panoramas (Radiance `.hdr`) that light the model and fill the backdrop of
the Showcase tab. `hdri.json` lists them; `src/libslic3r/Showcase/ShowcaseHdr.*` loads and bakes
them. All three are Poly Haven assets under CC0 1.0 (public domain, attribution not required;
https://polyhaven.com/license), the same sources `resources/textures/showcase/environments/README.md`
lists for the baked 128 px environments. Owner approved bundling them on 2026-11-01 (design D-R2);
the files are our own copies, never fetched from the Poly Haven API (D-R3).

| Id | File | Source page | Licence | Bytes | SHA-256 |
|---|---|---|---|---|---|
| `hdri_studio` | `studio_small_09_2k.hdr` | https://polyhaven.com/a/studio_small_09 | CC0 1.0 | 6,312,947 | `36724313c0fc66dab126ff90f081b85a0b1ef47be65eda1417bd20b033f0573f` |
| `hdri_outdoor` (has a sun) | `kloofendal_48d_partly_cloudy_puresky_2k.hdr` | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | CC0 1.0 | 5,451,493 | `5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198` |
| `hdri_workshop` (window light) | `artist_workshop_2k.hdr` | https://polyhaven.com/a/artist_workshop | CC0 1.0 | 6,756,758 | `09e871a2f2bb34395a6c21c9bea78b0e5d9cd3abff4d6da47c547da373342159` |

About 18.5 MB in all. `.gitattributes` marks `*.hdr` binary.

A file entry in `hdri.json` has `id`, `name`, `file`, `source`, `authors`, `licence`. An entry may
instead have `"procedural": "sky_sun" | "studio" | "interior"` (a panorama generated in code); the
unit tests use those generators and none is listed in the shipped manifest. If a listed file is missing
or unreadable the renderer logs a warning and the scene falls back to its backdrop colours.

# Showcase HDRI environments

Sharp 2k equirectangular panoramas (Radiance `.hdr`) that light the model and fill the backdrop of
the Showcase tab (`hdri.json` lists them; `src/libslic3r/Showcase/ShowcaseHdr.*` loads and bakes
them). A file entry looks like

    { "id": "hdri_studio", "name": "Studio softboxes", "file": "studio_small_09_2k.hdr",
      "source": "https://polyhaven.com/a/studio_small_09", "authors": "Poly Haven", "licence": "CC0 1.0" }

and a generated one has `"procedural": "sky_sun" | "studio" | "interior"` instead of `"file"`.

## Status (design D-R2, D-R3)

The three entries in `hdri.json` are generated in code (no file): stand-ins so the loader, the
prefilter, the sun light and the controls can be built and tested without fetching anything. The
photographs below are proposed to replace them; **nothing has been downloaded yet**. They are the
same Poly Haven CC0 sources `resources/textures/showcase/environments/README.md` already lists
for the baked 128 px environments, now as the sharp 2k originals.

| Slot | File | Source page | Licence | 2k size (estimate) |
|---|---|---|---|---|
| Studio | `studio_small_09_2k.hdr` | https://polyhaven.com/a/studio_small_09 | CC0 1.0 | about 5-7 MB |
| Outdoor (has a sun) | `kloofendal_48d_partly_cloudy_puresky_2k.hdr` | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | CC0 1.0 | about 5-7 MB |
| Interior-ish (window light) | `artist_workshop_2k.hdr` | https://polyhaven.com/a/artist_workshop | CC0 1.0 | about 5-7 MB |

Sizes are estimates from the design document (a 2k RGBE `.hdr` is ~5-7 MB), not measured.
Downloads for these come from our own mirror (the edgeslicer-deps release assets), never from the
Poly Haven API (D-R3). When the files are added: put them in this folder, replace the three generated
entries (or add the files next to them), and add the three ids' names to `showcase_hdri_name()` in
`src/slic3r/GUI/Showcase/ShowcaseTextures.cpp`.

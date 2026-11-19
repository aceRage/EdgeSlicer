#!/usr/bin/env python3
"""Showcase golden-image harness (1h-2b): makes fixtures/desk_photo_exif6.jpg, kept in the repository.

The desk photo of make_desk_photo.py stored the way a phone stores a portrait shot: the pixels turned
90 degrees counter-clockwise and an EXIF Orientation of 6 ("turn 90 clockwise to view"). Showcase must
show it upright (backplate_exif fixture). Needs Pillow, so the JPEG is made once and committed; the
harness itself does not run this.

    python make_exif_photo.py <desk_photo.png> <out.jpg>
"""
import sys

from PIL import Image


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    upright = Image.open(argv[1]).convert("RGB")
    stored = upright.rotate(90, expand=True)   # Pillow turns counter-clockwise
    exif = Image.Exif()
    exif[0x0112] = 6
    stored.save(argv[2], "JPEG", quality=88, exif=exif.tobytes())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

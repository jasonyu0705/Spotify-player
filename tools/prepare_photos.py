#!/usr/bin/env python3
"""Convert photos/ originals into flash-friendly data/photos/ baseline JPEGs."""

import argparse
from io import BytesIO
from pathlib import Path
import sys
import tempfile

from PIL import Image, ImageOps

PROJECT = Path(__file__).resolve().parents[1]
SCREEN_SIZE = (480, 320)
MAX_PHOTOS = 32
MAX_PHOTO_BYTES = 60 * 1024
# Leave room for LittleFS metadata in the existing 896 KiB partition.
MAX_DATA_BYTES = 700 * 1024
EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}


def encode_photo(path, crop=False):
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original)
        # Flatten transparency, including palette PNGs, onto the letterbox color.
        rgba = image.convert("RGBA")
        background = Image.new("RGBA", rgba.size, (18, 18, 18, 255))
        background.alpha_composite(rgba)
        image = background.convert("RGB")
        if crop:
            image = ImageOps.fit(image, SCREEN_SIZE, method=Image.Resampling.LANCZOS)
        else:
            image = ImageOps.pad(image, SCREEN_SIZE, method=Image.Resampling.LANCZOS,
                                 color=(18, 18, 18))
        for quality in (85, 80, 75, 70, 65, 60, 50, 40):
            output = BytesIO()
            # TJpg_Decoder requires baseline JPEG, not progressive JPEG.
            # Saving the newly created canvas also strips EXIF and GPS metadata.
            image.save(output, format="JPEG", quality=quality, optimize=True,
                       progressive=False, subsampling=2)
            data = output.getvalue()
            if len(data) <= MAX_PHOTO_BYTES:
                return data
    raise ValueError(f"Could not compress {path.name} below 60 KiB")


def prepare(source, output, crop=False):
    source, output = Path(source).resolve(), Path(output).resolve()
    if not source.is_dir():
        raise ValueError(f"Photo folder does not exist: {source}")
    # Never treat the output as originals or overwrite an input subfolder.
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("Source and output folders must be separate, non-nested folders")
    originals = sorted((p for p in source.iterdir()
                        if p.is_file() and not p.name.startswith(".")
                        and p.suffix.lower() in EXTENSIONS), key=lambda p: (p.name.casefold(), p.name))
    if not originals:
        raise ValueError(f"Add JPG, PNG, WebP, or BMP photos to {source} first. "
                         "For HEIC photos, export JPEG copies first.")
    if len(originals) > MAX_PHOTOS:
        raise ValueError(f"Found {len(originals)} photos; maximum is {MAX_PHOTOS}. "
                         "Around 10 is recommended for internal storage.")
    # Validate the entire batch before replacing any previously prepared images.
    encoded = [(p, encode_photo(p, crop)) for p in originals]
    other_bytes = 0
    if output.parent.exists():
        other_bytes = sum(p.stat().st_size for p in output.parent.rglob("*")
                          if p.is_file() and output not in p.parents)
    total = sum(len(data) for _, data in encoded)
    if total + other_bytes > MAX_DATA_BYTES:
        raise ValueError(f"Prepared photos and other data need {(total + other_bytes) / 1024:.0f} KiB; "
                         "the safe budget is 700 KiB. Remove some photos and try again.")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepared-", dir=output.parent) as temporary:
        staged = Path(temporary)
        for i, (_, data) in enumerate(encoded, 1):
            (staged / f"{i:03d}.jpg").write_bytes(data)
        # Only generated JPEGs are replaced; originals and other files are untouched.
        for old in output.iterdir():
            if old.is_file() and old.suffix.lower() in {".jpg", ".jpeg"}:
                old.unlink()
        for prepared in sorted(staged.iterdir()):
            prepared.replace(output / prepared.name)
    for i, (path, data) in enumerate(encoded, 1):
        print(f"{path.name} -> {i:03d}.jpg ({len(data) / 1024:.1f} KiB)")
    print(f"Prepared {len(encoded)} photos, {total / 1024:.1f} KiB total, in {output}")
    return len(encoded)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=PROJECT / "photos")
    parser.add_argument("--output", type=Path, default=PROJECT / "data" / "photos")
    parser.add_argument("--crop", action="store_true",
                        help="Fill the screen by cropping; default preserves the whole photo with borders")
    args = parser.parse_args()
    try:
        prepare(args.source, args.output, args.crop)
    except (ValueError, OSError, Image.DecompressionBombError) as error:
        parser.exit(1, f"Photo preparation failed: {error}\n")


if __name__ == "__main__":
    sys.exit(main())

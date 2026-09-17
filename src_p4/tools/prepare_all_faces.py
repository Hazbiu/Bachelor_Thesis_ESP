#!/usr/bin/env python3
"""Prepare every person's SD-card folder for ESP32 face enrollment.

Expected SD-card layout:

    /media/hazbiu/SDCARD/enroll/
    |-- Keti/
    |   |-- 001.rgb
    |   `-- ...
    `-- Muazzam/
        |-- 1.jpeg
        `-- ...

Each immediate subdirectory of ENROLL_ROOT is treated as one person. The
folder name is the person's display name. JPG, JPEG and PNG files are converted
to 320x240 RGB888 files named 001.rgb, 002.rgb, and so on. If a folder contains
only RGB files, those files are preserved and validated.

This script prepares enrollment images. The ESP32 firmware must scan these
folders to build /sdcard/FACE.DB and store the feature-ID-to-name mapping.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image, ImageOps, UnidentifiedImageError
except ImportError as exc:
    raise SystemExit(
        "Pillow is required. Install it with:\n"
        "  python3 -m pip install --user Pillow"
    ) from exc


# Only this path is hard-coded. Every person below it is discovered.
ENROLL_ROOT = Path("/media/hazbiu/SDCARD/enroll")

WIDTH = 320
HEIGHT = 240
EXPECTED_RGB_BYTES = WIDTH * HEIGHT * 3
SOURCE_SUFFIXES = {".jpg", ".jpeg", ".png"}
MAX_PERSON_NAME_BYTES = 31


def natural_sort_key(path: Path) -> list[object]:
    """Sort 2.jpeg before 10.jpeg."""
    return [
        int(part) if part.isdigit() else part.casefold()
        for part in re.split(r"(\d+)", path.name)
    ]


def validate_person_name(name: str) -> None:
    encoded = name.encode("utf-8")

    if not name or name in {".", ".."}:
        raise ValueError("the folder name is empty or invalid")

    if len(encoded) > MAX_PERSON_NAME_BYTES:
        raise ValueError(
            f"the UTF-8 name is {len(encoded)} bytes; maximum is "
            f"{MAX_PERSON_NAME_BYTES} bytes"
        )

    if any(character in name for character in (",", "\n", "\r")):
        raise ValueError("commas and line breaks are not allowed in a name")


def find_source_images(person_dir: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in person_dir.iterdir()
            if path.is_file() and path.suffix.casefold() in SOURCE_SUFFIXES
        ),
        key=natural_sort_key,
    )


def find_rgb_images(person_dir: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in person_dir.iterdir()
            if path.is_file() and path.suffix.casefold() == ".rgb"
        ),
        key=natural_sort_key,
    )


def validate_rgb_file(rgb_path: Path) -> None:
    actual_size = rgb_path.stat().st_size

    if actual_size != EXPECTED_RGB_BYTES:
        raise ValueError(
            f"{rgb_path.name} has {actual_size} bytes; "
            f"expected {EXPECTED_RGB_BYTES}"
        )


def convert_person_images(person_dir: Path, source_images: list[Path]) -> list[Path]:
    """Convert safely, preserving old RGB files if any conversion fails."""
    staging_dir = Path(tempfile.mkdtemp(prefix="rgb_staging_", dir=person_dir))

    try:
        staged_files: list[Path] = []

        for index, source_path in enumerate(source_images, start=1):
            output_path = staging_dir / f"{index:03d}.rgb"

            try:
                with Image.open(source_path) as image:
                    image = ImageOps.exif_transpose(image).convert("RGB")

                    # Crop to 4:3 without stretching the face.
                    image = ImageOps.fit(
                        image,
                        (WIDTH, HEIGHT),
                        method=Image.Resampling.LANCZOS,
                        centering=(0.5, 0.5),
                    )

                    output_path.write_bytes(image.tobytes())
            except (OSError, UnidentifiedImageError) as exc:
                raise ValueError(f"cannot read {source_path.name}: {exc}") from exc

            validate_rgb_file(output_path)
            staged_files.append(output_path)

        # Conversion of every source succeeded. Now replace old RGB outputs.
        for old_rgb in find_rgb_images(person_dir):
            old_rgb.unlink()

        final_files: list[Path] = []
        for staged_file in staged_files:
            final_path = person_dir / staged_file.name
            staged_file.replace(final_path)
            final_files.append(final_path)

        return final_files
    finally:
        shutil.rmtree(staging_dir, ignore_errors=True)


def prepare_person(person_dir: Path) -> int:
    validate_person_name(person_dir.name)

    source_images = find_source_images(person_dir)

    if source_images:
        print(f"  Source images: {len(source_images)}")
        rgb_images = convert_person_images(person_dir, source_images)
        print(f"  Converted RGB files: {len(rgb_images)}")
    else:
        rgb_images = find_rgb_images(person_dir)

        if not rgb_images:
            raise ValueError("no JPG, JPEG, PNG or RGB files found")

        for rgb_path in rgb_images:
            validate_rgb_file(rgb_path)

        print(f"  Existing valid RGB files: {len(rgb_images)}")

    for rgb_path in rgb_images:
        print(f"    {rgb_path.name}: {rgb_path.stat().st_size} bytes")

    return len(rgb_images)


def main() -> int:
    if not ENROLL_ROOT.is_dir():
        print(f"ERROR: enrollment folder does not exist: {ENROLL_ROOT}", file=sys.stderr)
        return 1

    person_dirs = sorted(
        (path for path in ENROLL_ROOT.iterdir() if path.is_dir()),
        key=lambda path: path.name.casefold(),
    )

    if not person_dirs:
        print(f"ERROR: no person folders found in {ENROLL_ROOT}", file=sys.stderr)
        return 1

    print(f"Enrollment root: {ENROLL_ROOT}")
    print(f"Detected people: {len(person_dirs)}")

    total_rgb_files = 0
    failed_people: list[str] = []

    for person_number, person_dir in enumerate(person_dirs, start=1):
        print(f"\n[{person_number}/{len(person_dirs)}] Person: {person_dir.name}")

        try:
            total_rgb_files += prepare_person(person_dir)
        except (OSError, ValueError) as exc:
            failed_people.append(person_dir.name)
            print(f"  ERROR: {exc}", file=sys.stderr)

    print("\nSummary")
    print(f"  People detected: {len(person_dirs)}")
    print(f"  Valid RGB files: {total_rgb_files}")

    if failed_people:
        print(f"  Failed people: {', '.join(failed_people)}", file=sys.stderr)
        print("Preparation failed. FACE.DB must not be rebuilt yet.", file=sys.stderr)
        return 1

    print("  Status: ready for ESP32 database building")
    print("\nPerson names are the folder names shown above.")
    print("The ESP32 firmware must now rebuild /sdcard/FACE.DB from these folders.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from pathlib import Path
from PIL import Image, ImageOps

WIDTH = 320
HEIGHT = 240

INPUT_ROOT_NAME = "face_images"
RAW_OUTPUT_ROOT_NAME = "raw_output"
PREVIEW_OUTPUT_ROOT_NAME = "preview_output"

SUPPORTED_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".bmp",
    ".webp",
}


def convert_image_keep_aspect(input_path: Path):
    img = Image.open(input_path)

    # Fix phone rotation
    img = ImageOps.exif_transpose(img)

    # Convert to RGB888
    img = img.convert("RGB")

    # Keep aspect ratio, fit inside 320x240
    src_w, src_h = img.size
    target_ratio = WIDTH / HEIGHT
    src_ratio = src_w / src_h

    if src_ratio > target_ratio:
        new_h = HEIGHT
        new_w = int(new_h * src_ratio)
    else:
        new_w = WIDTH
        new_h = int(new_w / src_ratio)

    img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)

    left = (new_w - WIDTH) // 2
    top = (new_h - HEIGHT) // 2

    img = img.crop((left, top, left + WIDTH, top + HEIGHT))

    return img


def main():
    script_dir = Path(__file__).resolve().parent

    input_root = script_dir / INPUT_ROOT_NAME
    raw_output_root = script_dir / RAW_OUTPUT_ROOT_NAME
    preview_output_root = script_dir / PREVIEW_OUTPUT_ROOT_NAME

    if not input_root.exists():
        raise RuntimeError(
            f"Input folder not found: {input_root}\n"
            f"Expected structure:\n"
            f"{INPUT_ROOT_NAME}/keti/photo1.jpeg"
        )

    raw_output_root.mkdir(exist_ok=True)
    preview_output_root.mkdir(exist_ok=True)

    user_folders = sorted([p for p in input_root.iterdir() if p.is_dir()])

    if not user_folders:
        raise RuntimeError(f"No user folders found inside {input_root}")

    for user_folder in user_folders:
        user_name = user_folder.name

        raw_user_output = raw_output_root / user_name
        preview_user_output = preview_output_root / user_name

        raw_user_output.mkdir(parents=True, exist_ok=True)
        preview_user_output.mkdir(parents=True, exist_ok=True)

        image_files = sorted(
            [
                p for p in user_folder.iterdir()
                if p.is_file() and p.suffix.lower() in SUPPORTED_EXTENSIONS
            ]
        )

        if not image_files:
            print(f"Skipping {user_name}: no images found")
            continue

        print(f"Converting user: {user_name}")

        for index, image_path in enumerate(image_files, start=1):
            img = convert_image_keep_aspect(image_path)

            raw_path = raw_user_output / f"{index:03d}.rgb"
            preview_path = preview_user_output / f"{index:03d}.png"

            raw_path.write_bytes(img.tobytes())
            img.save(preview_path)

            print(f"  {image_path.name} -> {raw_path}")
            print(f"  preview -> {preview_path}")

            expected_size = WIDTH * HEIGHT * 3
            actual_size = raw_path.stat().st_size

            if actual_size != expected_size:
                raise RuntimeError(
                    f"Wrong size for {raw_path}: "
                    f"got {actual_size}, expected {expected_size}"
                )

    print("\nDone.")
    print(f"Raw files: {raw_output_root}")
    print(f"Preview files: {preview_output_root}")


if __name__ == "__main__":
    main()

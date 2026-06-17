from pathlib import Path
from PIL import Image, ImageOps
import shutil

# Output image size expected by ESP32 database builder
WIDTH = 320
HEIGHT = 240

# Folder next to this script containing user folders
INPUT_ROOT_NAME = "face_images"

# Output folder created next to this script
OUTPUT_ROOT_NAME = "raw_output"

SUPPORTED_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".bmp",
    ".webp",
}


def convert_image(input_path: Path, output_path: Path) -> None:
    img = Image.open(input_path)

    # Fix phone/camera rotation
    img = ImageOps.exif_transpose(img)

    # Convert to RGB888
    img = img.convert("RGB")

    # Resize exactly to 320x240
    img = img.resize((WIDTH, HEIGHT))

    # Save raw RGB888 bytes
    output_path.write_bytes(img.tobytes())


def main() -> None:
    script_dir = Path(__file__).resolve().parent

    input_root = script_dir / INPUT_ROOT_NAME
    output_root = script_dir / OUTPUT_ROOT_NAME

    if not input_root.exists():
        raise RuntimeError(
            f"Input folder not found: {input_root}\n"
            f"Create it like this:\n"
            f"{INPUT_ROOT_NAME}/person_1/photo1.jpeg"
        )

    output_root.mkdir(exist_ok=True)

    user_folders = sorted([p for p in input_root.iterdir() if p.is_dir()])

    if not user_folders:
        raise RuntimeError(f"No user folders found inside {input_root}")

    for user_folder in user_folders:
        user_name = user_folder.name
        user_output = output_root / user_name

        # Clean old output for this user
        if user_output.exists():
            shutil.rmtree(user_output)

        user_output.mkdir(parents=True, exist_ok=True)

        image_files = sorted(
            [
                p for p in user_folder.iterdir()
                if p.is_file() and p.suffix.lower() in SUPPORTED_EXTENSIONS
            ]
        )

        if not image_files:
            print(f"Skipping {user_name}: no images found")
            continue

        print(f"Converting {user_name}: {len(image_files)} images")

        for index, image_path in enumerate(image_files, start=1):
            output_path = user_output / f"{index:03d}.rgb"
            convert_image(image_path, output_path)

            size = output_path.stat().st_size
            expected_size = WIDTH * HEIGHT * 3

            if size != expected_size:
                raise RuntimeError(
                    f"Wrong output size for {output_path}: "
                    f"got {size}, expected {expected_size}"
                )

            print(f"  {image_path.name} -> {output_path}")

    print("\nDone.")
    print(f"Raw files created in: {output_root}")
    print(f"Each file size should be: {WIDTH * HEIGHT * 3} bytes")


if __name__ == "__main__":
    main()

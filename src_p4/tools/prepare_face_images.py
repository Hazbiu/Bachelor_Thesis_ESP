from pathlib import Path
from PIL import Image, ImageOps

INPUT_DIR = Path("person_1_images")
OUTPUT_DIR = Path("sdcard/enroll/person_1")

WIDTH = 320
HEIGHT = 240

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

image_files = sorted(
    list(INPUT_DIR.glob("*.jpg")) +
    list(INPUT_DIR.glob("*.jpeg")) +
    list(INPUT_DIR.glob("*.png"))
)

if not image_files:
    raise RuntimeError("No JPG/PNG images found in person_1_images")

for index, image_path in enumerate(image_files, start=1):
    img = Image.open(image_path)

    # Fix phone image rotation using EXIF
    img = ImageOps.exif_transpose(img)

    # Convert to RGB888
    img = img.convert("RGB")

    # Resize to exact ESP32 builder size
    img = img.resize((WIDTH, HEIGHT))

    output_path = OUTPUT_DIR / f"{index:03d}.rgb"
    output_path.write_bytes(img.tobytes())

    print(f"Saved {output_path}")

print("Done.")

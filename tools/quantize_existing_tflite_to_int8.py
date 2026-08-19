#!/usr/bin/env python3
"""
Quantize existing FLOAT32 TFLite FlatBuffers directly to full INT8.

Input:
  /media/admin/SDCARD/models/FDET32.TFL
  /media/admin/SDCARD/models/FREC.TFL

Calibration images:
  /media/admin/SDCARD/enroll/**

Output:
  /media/admin/SDCARD/models/FDET8.TFL
  /media/admin/SDCARD/models/FREC8.TFL

Important:
- Uses TensorFlow Lite's internal Calibrator API, which accepts a TFLite
  FlatBuffer directly.
- Requests allow_float=False, int8 inputs/outputs, and int8 activations.
- Does NOT overwrite the existing models.
"""

from __future__ import annotations

import argparse
import inspect
import math
from pathlib import Path

import numpy as np
from PIL import Image

import tensorflow as tf
from tensorflow.lite.python.optimize import calibrator as tfl_calibrator
from tensorflow.python.framework import dtypes


RAW_RGB_112_BYTES = 112 * 112 * 3


def load_rgb_image(path: Path) -> np.ndarray:
    suffix = path.suffix.lower()

    if suffix == ".rgb":
        data = path.read_bytes()
        if len(data) != RAW_RGB_112_BYTES:
            raise ValueError(
                f"{path}: expected {RAW_RGB_112_BYTES} bytes for 112x112 RGB888, "
                f"got {len(data)}"
            )
        return np.frombuffer(data, dtype=np.uint8).reshape(112, 112, 3).copy()

    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def resize_rgb(img: np.ndarray, width: int, height: int) -> np.ndarray:
    im = Image.fromarray(img, mode="RGB")
    im = im.resize((width, height), Image.Resampling.BILINEAR)
    return np.asarray(im, dtype=np.uint8)


def normalize_minus1_plus1(img: np.ndarray) -> np.ndarray:
    return img.astype(np.float32) / 127.5 - 1.0


def discover_images(root: Path) -> list[Path]:
    allowed = {".jpg", ".jpeg", ".png", ".bmp", ".rgb"}
    paths = [p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in allowed]
    paths.sort()
    if not paths:
        raise RuntimeError(f"No calibration images found under {root}")
    return paths


def augmented_samples(paths: list[Path], width: int, height: int, target_count: int):
    """
    Deterministic calibration augmentation.

    Labels are not required for post-training calibration. We vary brightness,
    contrast, and horizontal orientation to cover a broader activation range
    than the small enrollment set alone.
    """
    base = [resize_rgb(load_rgb_image(p), width, height) for p in paths]

    brightness = [0.72, 0.86, 1.0, 1.14, 1.28]
    contrast = [0.85, 1.0, 1.15]

    produced = 0
    cycle = 0
    while produced < target_count:
        for i, img_u8 in enumerate(base):
            b = brightness[(cycle + i) % len(brightness)]
            c = contrast[(cycle * 2 + i) % len(contrast)]

            arr = img_u8.astype(np.float32)
            arr = (arr - 127.5) * c + 127.5
            arr = arr * b
            arr = np.clip(arr, 0, 255).astype(np.uint8)

            if (cycle + i) % 2:
                arr = arr[:, ::-1, :]

            sample = normalize_minus1_plus1(arr)[None, ...]
            yield [sample.astype(np.float32, copy=False)]
            produced += 1
            if produced >= target_count:
                return
        cycle += 1


def model_io(model_bytes: bytes):
    interpreter = tf.lite.Interpreter(model_content=model_bytes)
    interpreter.allocate_tensors()
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()
    return inputs, outputs, interpreter.get_tensor_details()


def print_model_summary(label: str, model_bytes: bytes):
    inputs, outputs, tensors = model_io(model_bytes)

    print(f"\n[{label}]")
    print(f"size={len(model_bytes)} bytes")
    for i, t in enumerate(inputs):
        print(
            f"input[{i}] name={t['name']} shape={list(t['shape'])} "
            f"dtype={np.dtype(t['dtype']).name} quant={t['quantization']}"
        )
    for i, t in enumerate(outputs):
        print(
            f"output[{i}] name={t['name']} shape={list(t['shape'])} "
            f"dtype={np.dtype(t['dtype']).name} quant={t['quantization']}"
        )

    float_tensors = [
        t["name"] for t in tensors
        if np.dtype(t["dtype"]) in (np.dtype(np.float32), np.dtype(np.float64))
    ]
    print(f"floating_point_tensor_count={len(float_tensors)}")
    if float_tensors:
        print("first_float_tensors:")
        for name in float_tensors[:20]:
            print(f"  {name}")


def direct_full_int8_quantize(
    model_bytes: bytes,
    dataset_gen,
) -> bytes:
    # This mirrors the calibration stage used by TensorFlow's TFLite converter:
    # add intermediate tensors, then calibrate + quantize.
    prepared = tfl_calibrator.add_intermediate_tensors(model_bytes)
    cal = tfl_calibrator.Calibrator(prepared)

    fn = cal.calibrate_and_quantize
    sig = inspect.signature(fn)
    kwargs = {
        "dataset_gen": dataset_gen,
        "input_type": dtypes.int8,
        "output_type": dtypes.int8,
        "allow_float": False,
        "activations_type": dtypes.int8,
    }

    # Older/newer TensorFlow releases differ slightly in optional parameters.
    if "resize_input" in sig.parameters:
        kwargs["resize_input"] = False
    if "disable_per_channel" in sig.parameters:
        kwargs["disable_per_channel"] = False
    if "disable_per_channel_quantization_for_dense_layers" in sig.parameters:
        kwargs["disable_per_channel_quantization_for_dense_layers"] = False

    try:
        return fn(**kwargs)
    except TypeError:
        # Fallback for builds that do not expose keyword names identically.
        return fn(
            dataset_gen,
            dtypes.int8,
            dtypes.int8,
            False,
            dtypes.int8,
        )


def quantize_one(
    src: Path,
    dst: Path,
    calibration_paths: list[Path],
    samples: int,
):
    model = src.read_bytes()
    inputs, outputs, _ = model_io(model)

    if len(inputs) != 1:
        raise RuntimeError(f"{src}: expected 1 input, got {len(inputs)}")

    shape = list(inputs[0]["shape"])
    if len(shape) != 4 or shape[0] != 1 or shape[3] != 3:
        raise RuntimeError(f"{src}: unsupported input shape {shape}")

    height = int(shape[1])
    width = int(shape[2])

    print_model_summary(f"SOURCE {src.name}", model)
    print(
        f"\nQuantizing {src.name} -> {dst.name} "
        f"using {samples} representative samples at {width}x{height}..."
    )

    def rep_gen():
        yield from augmented_samples(
            calibration_paths, width, height, samples
        )

    quant = direct_full_int8_quantize(model, rep_gen)
    dst.write_bytes(quant)

    print_model_summary(f"INT8 {dst.name}", quant)

    q_inputs, q_outputs, q_tensors = model_io(quant)

    if np.dtype(q_inputs[0]["dtype"]) != np.dtype(np.int8):
        raise RuntimeError(f"{dst}: input is not INT8")
    for out in q_outputs:
        if np.dtype(out["dtype"]) != np.dtype(np.int8):
            raise RuntimeError(f"{dst}: output {out['name']} is not INT8")

    # FLOAT32 tensors indicate the graph was not fully integerized.
    float_tensors = [
        t["name"] for t in q_tensors
        if np.dtype(t["dtype"]) in (np.dtype(np.float32), np.dtype(np.float64))
    ]
    if float_tensors:
        raise RuntimeError(
            f"{dst}: graph still contains {len(float_tensors)} floating-point tensors; "
            "not a strict full-INT8 model"
        )

    print(f"[OK] Strict INT8 model written: {dst}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-dir",
        default="/media/admin/SDCARD/models",
        help="Directory containing FDET32.TFL and FREC.TFL",
    )
    parser.add_argument(
        "--calibration-dir",
        default="/media/admin/SDCARD/enroll",
        help="Representative image root",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=160,
        help="Representative samples generated per model",
    )
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    calibration_dir = Path(args.calibration_dir)

    det_src = model_dir / "FDET32.TFL"
    rec_src = model_dir / "FREC.TFL"
    det_dst = model_dir / "FDET8.TFL"
    rec_dst = model_dir / "FREC8.TFL"

    for p in (det_src, rec_src):
        if not p.is_file():
            raise FileNotFoundError(p)

    calibration_paths = discover_images(calibration_dir)
    print(f"Calibration images discovered: {len(calibration_paths)}")
    for p in calibration_paths[:20]:
        print(f"  {p}")
    if len(calibration_paths) > 20:
        print(f"  ... and {len(calibration_paths) - 20} more")

    # Never overwrite old INT8 candidates silently.
    for p in (det_dst, rec_dst):
        if p.exists():
            backup = p.with_suffix(p.suffix + ".bak")
            if backup.exists():
                backup.unlink()
            p.rename(backup)
            print(f"Existing {p.name} moved to {backup.name}")

    quantize_one(det_src, det_dst, calibration_paths, args.samples)
    quantize_one(rec_src, rec_dst, calibration_paths, args.samples)

    print("\n============================================================")
    print("DONE")
    print("============================================================")
    print(det_dst)
    print(rec_dst)
    print("\nNext step: patch firmware backend to load FDET8.TFL/FREC8.TFL")
    print("and feed/dequantize INT8 tensors for ESP-NN.")
    print("Do NOT delete or overwrite FDET32.TFL/FREC.TFL.")


if __name__ == "__main__":
    main()

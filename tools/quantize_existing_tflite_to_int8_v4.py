#!/usr/bin/env python3
"""
v4: Quantize existing FLOAT32 TFLite FlatBuffers directly to full INT8.

Fixes carried from v2:
FDET32.TFL was created by removing FP16 DEQUANTIZE operators from FDET.TFL.
That transformation can leave an UNUSED DEQUANTIZE OperatorCode entry in the
FlatBuffer operator-code table. TensorFlow's calibration wrapper tries to
register every OperatorCode in that table, even when no operator references it,
and therefore rejected the stale DEQUANTIZE v2 registration.

v3 added support for this project's 320x240 raw RGB888 enrollment files.\n\nv4 removes calibration-only unreferenced FLOAT32 tensors before strict validation.\n\nThis script first compacts/remaps the OperatorCode table so only codes actually
referenced by operators remain. It then performs calibration + strict INT8
quantization.

Inputs:
  /media/admin/SDCARD/models/FDET32.TFL
  /media/admin/SDCARD/models/FREC.TFL

Calibration:
  /media/admin/SDCARD/enroll/**

Outputs:
  /media/admin/SDCARD/models/FDET8.TFL
  /media/admin/SDCARD/models/FREC8.TFL

Existing models are never overwritten.
"""

from __future__ import annotations

import argparse
import inspect
from pathlib import Path

import numpy as np
from PIL import Image

import tensorflow as tf
from tensorflow.lite.python.optimize import calibrator as tfl_calibrator
from tensorflow.lite.python import schema_py_generated as schema_fb
from tensorflow.lite.tools import flatbuffer_utils
from tensorflow.python.framework import dtypes


# Raw enrollment RGB888 files in this project are normally 320x240:
# 320 * 240 * 3 = 230400 bytes.
#
# Keep a few known sizes so the calibration tool can also consume future
# pre-cropped RGB888 samples without guessing dimensions.
RAW_RGB_DIMENSIONS = {
    112 * 112 * 3: (112, 112),
    128 * 128 * 3: (128, 128),
    320 * 240 * 3: (320, 240),
    640 * 480 * 3: (640, 480),
}


def enum_name(enum_cls, value: int) -> str:
    for name, enum_value in vars(enum_cls).items():
        if name.startswith("_"):
            continue
        if isinstance(enum_value, int) and enum_value == value:
            return name
    return str(value)


def operator_builtin_code(code_obj) -> int:
    """
    Modern ModelT OperatorCode objects expose builtinCode. For compatibility
    with older generated schemas, fall back to deprecatedBuiltinCode when
    needed.
    """
    builtin = int(getattr(code_obj, "builtinCode", 0))
    deprecated = int(getattr(code_obj, "deprecatedBuiltinCode", 0))

    placeholder = int(
        getattr(schema_fb.BuiltinOperator, "PLACEHOLDER_FOR_GREATER_OP_CODES", 127)
    )

    if builtin >= placeholder:
        return builtin

    # In modern models builtinCode is also populated for normal builtin ops.
    # Prefer it when non-zero. CUSTOM can legitimately be 32.
    if builtin != 0:
        return builtin

    return deprecated


def describe_operator_codes(model_bytes: bytes, label: str) -> None:
    model = flatbuffer_utils.convert_bytearray_to_object(bytearray(model_bytes))

    used = set()
    for subgraph in model.subgraphs or []:
        for op in subgraph.operators or []:
            used.add(int(op.opcodeIndex))

    print(f"\n[{label}] operator_code_table={len(model.operatorCodes or [])} used={len(used)}")
    for index, code in enumerate(model.operatorCodes or []):
        builtin = operator_builtin_code(code)
        name = enum_name(schema_fb.BuiltinOperator, builtin)
        version = int(getattr(code, "version", 1))
        marker = "USED" if index in used else "UNUSED"
        print(f"  opcode[{index:02d}] {name:<28} version={version:<2d} {marker}")


def compact_unused_operator_codes(model_bytes: bytes, label: str) -> bytes:
    """
    Remove unreferenced OperatorCode records and remap every Operator.opcodeIndex.

    This is semantics-preserving: no operator is removed or changed. Only dead
    entries in the global operator-code lookup table are deleted.
    """
    model = flatbuffer_utils.convert_bytearray_to_object(bytearray(model_bytes))

    if not model.operatorCodes:
        return model_bytes

    used = set()
    for subgraph in model.subgraphs or []:
        for op in subgraph.operators or []:
            idx = int(op.opcodeIndex)
            if idx < 0 or idx >= len(model.operatorCodes):
                raise RuntimeError(
                    f"{label}: operator references invalid opcode index {idx}"
                )
            used.add(idx)

    ordered_used = sorted(used)
    mapping = {old: new for new, old in enumerate(ordered_used)}

    removed = []
    for idx, code in enumerate(model.operatorCodes):
        if idx not in used:
            builtin = operator_builtin_code(code)
            removed.append(
                (
                    idx,
                    enum_name(schema_fb.BuiltinOperator, builtin),
                    int(getattr(code, "version", 1)),
                )
            )

    if not removed:
        print(f"[{label}] No unused OperatorCode records found.")
        return model_bytes

    print(f"[{label}] Removing {len(removed)} unused OperatorCode record(s):")
    for idx, name, version in removed:
        print(f"  remove opcode[{idx}] {name} version={version}")

    model.operatorCodes = [model.operatorCodes[i] for i in ordered_used]

    for subgraph in model.subgraphs or []:
        for op in subgraph.operators or []:
            op.opcodeIndex = mapping[int(op.opcodeIndex)]

    compacted = bytes(flatbuffer_utils.convert_object_to_bytearray(model))

    # Verify normal TensorFlow Lite can still allocate the exact compacted graph.
    interpreter = tf.lite.Interpreter(model_content=compacted)
    interpreter.allocate_tensors()

    print(
        f"[{label}] OperatorCode table compacted: "
        f"{len(ordered_used) + len(removed)} -> {len(ordered_used)}"
    )
    return compacted



def _remap_tensor_index(index: int, mapping: dict[int, int]) -> int:
    index = int(index)
    if index < 0:
        return index
    if index not in mapping:
        raise RuntimeError(
            f"Tensor index {index} is referenced but was not retained"
        )
    return mapping[index]


def _remap_tensor_index_sequence(values, mapping: dict[int, int]):
    if values is None:
        return values
    return [
        _remap_tensor_index(value, mapping)
        for value in values
    ]


def compact_unused_tensors(model_bytes: bytes, label: str) -> bytes:
    """
    Remove tensors that are not referenced by the executable graph.

    TensorFlow's calibration path can leave diagnostic/intermediate FLOAT32
    tensors in SubGraph.tensors even though no operator, model input, or model
    output references them. They do not participate in inference, but a naive
    "all tensors must be INT8" check sees them and reports a false failure.

    This compactor keeps every tensor referenced by:
      - subgraph inputs
      - subgraph outputs
      - operator inputs
      - operator outputs
      - operator intermediates

    It then remaps tensor indices consistently. No operator or tensor used by
    inference is removed.
    """
    model = flatbuffer_utils.convert_bytearray_to_object(
        bytearray(model_bytes)
    )

    total_before = 0
    total_after = 0
    removed_float_names = []

    for subgraph_index, subgraph in enumerate(model.subgraphs or []):
        tensors = list(subgraph.tensors or [])
        total_before += len(tensors)

        referenced = set()

        for value in subgraph.inputs or []:
            if int(value) >= 0:
                referenced.add(int(value))

        for value in subgraph.outputs or []:
            if int(value) >= 0:
                referenced.add(int(value))

        for op in subgraph.operators or []:
            for sequence in (
                getattr(op, "inputs", None),
                getattr(op, "outputs", None),
                getattr(op, "intermediates", None),
            ):
                if sequence is None:
                    continue
                for value in sequence:
                    if int(value) >= 0:
                        referenced.add(int(value))

        invalid = [
            index for index in referenced
            if index < 0 or index >= len(tensors)
        ]
        if invalid:
            raise RuntimeError(
                f"{label}: subgraph {subgraph_index} has invalid "
                f"tensor references: {invalid}"
            )

        kept_old_indices = sorted(referenced)
        mapping = {
            old_index: new_index
            for new_index, old_index in enumerate(kept_old_indices)
        }

        for old_index, tensor in enumerate(tensors):
            if old_index in referenced:
                continue

            tensor_type = int(getattr(tensor, "type", -1))
            if tensor_type == int(schema_fb.TensorType.FLOAT32):
                removed_float_names.append(
                    getattr(tensor, "name", b"")
                )

        subgraph.tensors = [
            tensors[old_index]
            for old_index in kept_old_indices
        ]
        total_after += len(subgraph.tensors)

        subgraph.inputs = _remap_tensor_index_sequence(
            subgraph.inputs,
            mapping,
        )
        subgraph.outputs = _remap_tensor_index_sequence(
            subgraph.outputs,
            mapping,
        )

        for op in subgraph.operators or []:
            op.inputs = _remap_tensor_index_sequence(
                getattr(op, "inputs", None),
                mapping,
            )
            op.outputs = _remap_tensor_index_sequence(
                getattr(op, "outputs", None),
                mapping,
            )
            if getattr(op, "intermediates", None) is not None:
                op.intermediates = _remap_tensor_index_sequence(
                    op.intermediates,
                    mapping,
                )

        # SignatureDefs are uncommon in these microcontroller models, but if
        # present they also hold tensor indices into the referenced subgraph.
        for signature in getattr(model, "signatureDefs", None) or []:
            if int(getattr(signature, "subgraphIndex", 0)) != subgraph_index:
                continue

            for tensor_map in (
                list(getattr(signature, "inputs", None) or []) +
                list(getattr(signature, "outputs", None) or [])
            ):
                tensor_map.tensorIndex = _remap_tensor_index(
                    tensor_map.tensorIndex,
                    mapping,
                )

    compacted = bytes(
        flatbuffer_utils.convert_object_to_bytearray(model)
    )

    # Verify the compacted FlatBuffer is executable by a normal TFLite
    # interpreter before continuing.
    interpreter = tf.lite.Interpreter(model_content=compacted)
    interpreter.allocate_tensors()

    print(
        f"[{label}] Unused tensor compaction: "
        f"{total_before} -> {total_after}; "
        f"removed={total_before - total_after}"
    )

    if removed_float_names:
        print(
            f"[{label}] Removed "
            f"{len(removed_float_names)} unreferenced FLOAT32 tensors"
        )
        for name in removed_float_names[:20]:
            if isinstance(name, bytes):
                name = name.decode("utf-8", errors="replace")
            print(f"  removed unused float tensor: {name}")
        if len(removed_float_names) > 20:
            print(
                f"  ... and {len(removed_float_names) - 20} more"
            )

    return compacted

def load_rgb_image(path: Path) -> np.ndarray:
    suffix = path.suffix.lower()

    if suffix == ".rgb":
        data = path.read_bytes()
        dims = RAW_RGB_DIMENSIONS.get(len(data))

        if dims is None:
            known = ", ".join(
                f"{w}x{h}={size}B"
                for size, (w, h) in sorted(RAW_RGB_DIMENSIONS.items())
            )
            raise ValueError(
                f"{path}: unsupported raw RGB888 size {len(data)} bytes. "
                f"Known layouts: {known}"
            )

        width, height = dims
        print(
            f"[CALIBRATION] raw RGB888 {path.name}: "
            f"{width}x{height} ({len(data)} bytes)"
        )

        return (
            np.frombuffer(data, dtype=np.uint8)
            .reshape(height, width, 3)
            .copy()
        )

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
    paths = [
        p for p in root.rglob("*")
        if p.is_file() and p.suffix.lower() in allowed
    ]
    paths.sort()
    if not paths:
        raise RuntimeError(f"No calibration images found under {root}")
    return paths


def augmented_samples(
    paths: list[Path],
    width: int,
    height: int,
    target_count: int,
):
    """
    Deterministic representative calibration stream.

    Calibration does not need labels. Brightness/contrast/mirroring expands the
    observed activation range from the small enrollment set without changing the
    source files.
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

            # Matches the current TFLM FP32 runtime convention.
            sample = normalize_minus1_plus1(arr)[None, ...]
            yield [sample.astype(np.float32, copy=False)]

            produced += 1
            if produced >= target_count:
                return

        cycle += 1


def model_io(model_bytes: bytes):
    interpreter = tf.lite.Interpreter(model_content=model_bytes)
    interpreter.allocate_tensors()
    return (
        interpreter.get_input_details(),
        interpreter.get_output_details(),
        interpreter.get_tensor_details(),
    )


def print_model_summary(label: str, model_bytes: bytes) -> None:
    inputs, outputs, tensors = model_io(model_bytes)

    print(f"\n[{label}]")
    print(f"size={len(model_bytes)} bytes")

    for i, tensor in enumerate(inputs):
        print(
            f"input[{i}] name={tensor['name']} "
            f"shape={list(tensor['shape'])} "
            f"dtype={np.dtype(tensor['dtype']).name} "
            f"quant={tensor['quantization']}"
        )

    for i, tensor in enumerate(outputs):
        print(
            f"output[{i}] name={tensor['name']} "
            f"shape={list(tensor['shape'])} "
            f"dtype={np.dtype(tensor['dtype']).name} "
            f"quant={tensor['quantization']}"
        )

    float_tensors = [
        tensor["name"]
        for tensor in tensors
        if np.dtype(tensor["dtype"]) in (
            np.dtype(np.float32),
            np.dtype(np.float64),
        )
    ]

    print(f"floating_point_tensor_count={len(float_tensors)}")


def direct_full_int8_quantize(model_bytes: bytes, dataset_gen) -> bytes:
    """
    TensorFlow's normal converter also inserts intermediate calibration tensors
    before constructing Calibrator. We follow the same internal pipeline.
    """
    prepared = tfl_calibrator.add_intermediate_tensors(model_bytes)

    # Defensive second compaction in case the preparatory pass preserves/adds
    # another unreferenced OperatorCode entry.
    prepared = compact_unused_operator_codes(
        prepared,
        "CALIBRATION-PREPARED",
    )

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

    # TensorFlow versions expose slightly different optional arguments.
    if "resize_input" in sig.parameters:
        kwargs["resize_input"] = False
    if "disable_per_channel" in sig.parameters:
        kwargs["disable_per_channel"] = False
    if "disable_per_channel_quantization_for_dense_layers" in sig.parameters:
        kwargs["disable_per_channel_quantization_for_dense_layers"] = False

    try:
        return bytes(fn(**kwargs))
    except TypeError:
        # Compatibility fallback for older wrapper signatures.
        return bytes(
            fn(
                dataset_gen,
                dtypes.int8,
                dtypes.int8,
                False,
                dtypes.int8,
            )
        )


def verify_strict_int8(dst: Path, model_bytes: bytes) -> None:
    inputs, outputs, tensors = model_io(model_bytes)

    for tensor in inputs:
        if np.dtype(tensor["dtype"]) != np.dtype(np.int8):
            raise RuntimeError(
                f"{dst}: input {tensor['name']} is not INT8"
            )

    for tensor in outputs:
        if np.dtype(tensor["dtype"]) != np.dtype(np.int8):
            raise RuntimeError(
                f"{dst}: output {tensor['name']} is not INT8"
            )

    float_tensors = [
        tensor["name"]
        for tensor in tensors
        if np.dtype(tensor["dtype"]) in (
            np.dtype(np.float32),
            np.dtype(np.float64),
        )
    ]

    if float_tensors:
        print(f"\n[FAIL] {dst.name} still has floating-point tensors:")
        for name in float_tensors[:50]:
            print(f"  {name}")
        raise RuntimeError(
            f"{dst}: graph still contains "
            f"{len(float_tensors)} floating-point tensors; "
            "not a strict full-INT8 model"
        )


def quantize_one(
    src: Path,
    dst: Path,
    calibration_paths: list[Path],
    samples: int,
) -> None:
    original = src.read_bytes()

    print_model_summary(f"SOURCE {src.name}", original)
    describe_operator_codes(original, f"SOURCE {src.name}")

    # Critical v2 fix.
    sanitized = compact_unused_operator_codes(
        original,
        f"SANITIZE {src.name}",
    )

    describe_operator_codes(sanitized, f"SANITIZED {src.name}")

    inputs, _, _ = model_io(sanitized)
    if len(inputs) != 1:
        raise RuntimeError(f"{src}: expected exactly 1 input")

    shape = [int(v) for v in inputs[0]["shape"]]
    if len(shape) != 4 or shape[0] != 1 or shape[3] != 3:
        raise RuntimeError(f"{src}: unsupported input shape {shape}")

    height = shape[1]
    width = shape[2]

    print(
        f"\nQuantizing {src.name} -> {dst.name} "
        f"using {samples} samples at {width}x{height}..."
    )

    def rep_gen():
        yield from augmented_samples(
            calibration_paths,
            width,
            height,
            samples,
        )

    quantized = direct_full_int8_quantize(sanitized, rep_gen)

    print_model_summary(f"INT8 RAW {dst.name}", quantized)
    describe_operator_codes(quantized, f"INT8 RAW {dst.name}")

    # The old v3 verifier counted calibration-only FLOAT32 tensors that were
    # present in the tensor table but referenced by no executable operator.
    # Remove those dead tensors, then perform the strict validation on the
    # actual executable graph.
    quantized = compact_unused_tensors(
        quantized,
        f"INT8-COMPACT {dst.name}",
    )

    print_model_summary(f"INT8 FINAL {dst.name}", quantized)
    describe_operator_codes(quantized, f"INT8 FINAL {dst.name}")
    verify_strict_int8(dst, quantized)

    # Only write after all validation passes.
    dst.write_bytes(quantized)
    print(f"\n[OK] Strict executable INT8 model written: {dst}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-dir",
        default="/media/admin/SDCARD/models",
    )
    parser.add_argument(
        "--calibration-dir",
        default="/media/admin/SDCARD/enroll",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=160,
    )
    args = parser.parse_args()

    model_dir = Path(args.model_dir)
    calibration_dir = Path(args.calibration_dir)

    det_src = model_dir / "FDET32.TFL"
    rec_src = model_dir / "FREC.TFL"
    det_dst = model_dir / "FDET8.TFL"
    rec_dst = model_dir / "FREC8.TFL"

    for src in (det_src, rec_src):
        if not src.is_file():
            raise FileNotFoundError(src)

    calibration_paths = discover_images(calibration_dir)

    print(f"TensorFlow version: {tf.__version__}")
    print(f"Calibration images discovered: {len(calibration_paths)}")
    for path in calibration_paths[:20]:
        print(f"  {path}")
    if len(calibration_paths) > 20:
        print(f"  ... and {len(calibration_paths) - 20} more")

    # Preserve any previous experiment result rather than overwriting it.
    for dst in (det_dst, rec_dst):
        if dst.exists():
            backup = dst.with_name(dst.name + ".PREV")
            if backup.exists():
                backup.unlink()
            dst.rename(backup)
            print(f"Previous {dst.name} moved to {backup.name}")

    quantize_one(
        det_src,
        det_dst,
        calibration_paths,
        args.samples,
    )

    quantize_one(
        rec_src,
        rec_dst,
        calibration_paths,
        args.samples,
    )

    print("\n============================================================")
    print("SUCCESS: BOTH STRICT INT8 MODELS CREATED")
    print("============================================================")
    print(det_dst)
    print(rec_dst)


if __name__ == "__main__":
    main()

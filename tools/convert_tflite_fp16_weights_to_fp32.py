#!/usr/bin/env python3

import argparse
import os
import numpy as np
import tensorflow as tf

from tensorflow.lite.tools import flatbuffer_utils
from tensorflow.lite.python import schema_py_generated as schema_fb


def op_name(model, op):
    return flatbuffer_utils.opcode_to_name(model, op.opcodeIndex)


def convert_model(src, dst):
    print(f"[INPUT]  {src}")
    print(f"[OUTPUT] {dst}")

    try:
        model = flatbuffer_utils.read_model_with_mutable_tensors(src)
    except Exception:
        model = flatbuffer_utils.read_model(src)

    converted_buffers = set()
    removed_dequantize = 0

    for sg_index, subgraph in enumerate(model.subgraphs):
        remove_ids = set()

        for op_index, op in enumerate(subgraph.operators):
            if op_name(model, op) != "DEQUANTIZE":
                continue

            if len(op.inputs) != 1 or len(op.outputs) != 1:
                raise RuntimeError(
                    f"Unexpected DEQUANTIZE contract at "
                    f"subgraph={sg_index} op={op_index}"
                )

            input_idx = int(op.inputs[0])
            output_idx = int(op.outputs[0])

            input_tensor = subgraph.tensors[input_idx]
            output_tensor = subgraph.tensors[output_idx]

            if output_tensor.type != schema_fb.TensorType.FLOAT32:
                raise RuntimeError(
                    f"DEQUANTIZE output is not FLOAT32 at op {op_index}"
                )

            if input_tensor.type == schema_fb.TensorType.FLOAT16:
                buffer_idx = int(input_tensor.buffer)

                if buffer_idx <= 0:
                    raise RuntimeError(
                        f"FLOAT16 DEQUANTIZE input is not constant "
                        f"at op {op_index}"
                    )

                buffer = model.buffers[buffer_idx]
                raw = bytes(buffer.data)

                if not raw:
                    raise RuntimeError(
                        f"Empty constant buffer at op {op_index}"
                    )

                if buffer_idx not in converted_buffers:
                    values16 = np.frombuffer(raw, dtype="<f2")

                    expected_elements = int(
                        np.prod(np.asarray(input_tensor.shape, dtype=np.int64))
                    )

                    if values16.size != expected_elements:
                        raise RuntimeError(
                            f"Buffer element mismatch at op {op_index}: "
                            f"buffer={values16.size} "
                            f"shape={expected_elements}"
                        )

                    values32 = values16.astype("<f4")

                    buffer.data = bytearray(values32.tobytes())
                    input_tensor.type = schema_fb.TensorType.FLOAT32

                    converted_buffers.add(buffer_idx)

                    name = input_tensor.name
                    if isinstance(name, bytes):
                        name = name.decode("utf-8", errors="replace")

                    print(
                        f"[CONVERT] tensor={name} "
                        f"elements={values16.size} "
                        f"FP16 -> FP32"
                    )

            elif input_tensor.type != schema_fb.TensorType.FLOAT32:
                raise RuntimeError(
                    f"Unexpected DEQUANTIZE input tensor type "
                    f"{input_tensor.type} at op {op_index}"
                )

            # Replace every use of the DEQUANTIZE output with
            # the now-FP32 constant input tensor.
            for consumer in subgraph.operators:
                consumer.inputs = [
                    input_idx if int(x) == output_idx else int(x)
                    for x in consumer.inputs
                ]

            subgraph.outputs = [
                input_idx if int(x) == output_idx else int(x)
                for x in subgraph.outputs
            ]

            remove_ids.add(id(op))
            removed_dequantize += 1

        subgraph.operators = [
            op for op in subgraph.operators
            if id(op) not in remove_ids
        ]

    if removed_dequantize == 0:
        raise RuntimeError("No FLOAT16 DEQUANTIZE graph was found")

    flatbuffer_utils.write_model(model, dst)

    print()
    print(f"[DONE] Converted constant buffers : {len(converted_buffers)}")
    print(f"[DONE] Removed DEQUANTIZE ops     : {removed_dequantize}")
    print(f"[DONE] New model size             : {os.path.getsize(dst)} bytes")


def inspect(path):
    interpreter = tf.lite.Interpreter(
        model_path=path,
        experimental_preserve_all_tensors=True
    )
    interpreter.allocate_tensors()

    ops = interpreter._get_ops_details()
    tensors = interpreter.get_tensor_details()

    dequant_count = sum(
        1 for op in ops if op["op_name"] == "DEQUANTIZE"
    )

    fp16_count = sum(
        1 for tensor in tensors
        if tensor["dtype"] == np.float16
    )

    print()
    print(f"[VERIFY] model={path}")
    print(f"[VERIFY] operators={len(ops)}")
    print(f"[VERIFY] DEQUANTIZE operators={dequant_count}")
    print(f"[VERIFY] FLOAT16 tensors={fp16_count}")

    print("[VERIFY] inputs:")
    for tensor in interpreter.get_input_details():
        print(
            "   ",
            tensor["name"],
            tensor["shape"],
            tensor["dtype"]
        )

    print("[VERIFY] outputs:")
    for tensor in interpreter.get_output_details():
        print(
            "   ",
            tensor["name"],
            tensor["shape"],
            tensor["dtype"]
        )

    return dequant_count, fp16_count


def compare(original, converted):
    old = tf.lite.Interpreter(model_path=original)
    new = tf.lite.Interpreter(model_path=converted)

    old.allocate_tensors()
    new.allocate_tensors()

    old_input = old.get_input_details()[0]
    new_input = new.get_input_details()[0]

    if not np.array_equal(old_input["shape"], new_input["shape"]):
        raise RuntimeError("Input shapes changed")

    rng = np.random.default_rng(12345)

    max_error = 0.0

    for test_index in range(3):
        x = rng.uniform(
            -1.0,
            1.0,
            size=tuple(old_input["shape"])
        ).astype(np.float32)

        old.set_tensor(old_input["index"], x)
        new.set_tensor(new_input["index"], x)

        old.invoke()
        new.invoke()

        old_outputs = old.get_output_details()
        new_outputs = new.get_output_details()

        if len(old_outputs) != len(new_outputs):
            raise RuntimeError("Output count changed")

        for old_desc, new_desc in zip(old_outputs, new_outputs):
            a = old.get_tensor(old_desc["index"]).astype(np.float32)
            b = new.get_tensor(new_desc["index"]).astype(np.float32)

            if a.shape != b.shape:
                raise RuntimeError("Output shape changed")

            error = float(np.max(np.abs(a - b)))
            max_error = max(max_error, error)

        print(
            f"[COMPARE] test={test_index + 1} "
            f"current_max_abs_error={max_error:.9g}"
        )

    print()
    print(f"[COMPARE] FINAL MAX ABS ERROR = {max_error:.9g}")

    if max_error > 1e-5:
        raise RuntimeError(
            f"Converted model differs too much: {max_error}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()

    convert_model(args.input, args.output)

    dequant_count, fp16_count = inspect(args.output)

    if dequant_count != 0:
        raise RuntimeError(
            f"Converted model still has {dequant_count} DEQUANTIZE ops"
        )

    if fp16_count != 0:
        raise RuntimeError(
            f"Converted model still has {fp16_count} FLOAT16 tensors"
        )

    compare(args.input, args.output)

    print()
    print("======================================================")
    print("SUCCESS: detector is now an FP32-weight TFLite model")
    print("======================================================")


if __name__ == "__main__":
    main()

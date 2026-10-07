# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Writes a tiny ONNX model with the same interface and metadata as the exported backbone.

Used as a test fixture for the C++ ONNX Runtime adapter, so its tests do not need the large
backbone. The model averages the normalised input over all pixels and channels and repeats the
value for every feature part (dims values per part).

Usage (from the training directory):
    uv run python -m ttrally_training.make_test_model --out ../tests/fixtures/tiny_backbone.onnx
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import onnx
from onnx import TensorProto, helper

from ttrally_training.backbone import FEATURE_PARTS, IMAGENET_MEAN, IMAGENET_STD

HEIGHT, WIDTH, PART_DIMS, OPSET = 28, 42, 2, 18


def build() -> onnx.ModelProto:
    total = len(FEATURE_PARTS) * PART_DIMS
    nodes = [
        helper.make_node("ReduceMean", ["pixel_values", "axes"], ["pooled"], keepdims=0),
        helper.make_node("Unsqueeze", ["pooled", "one"], ["column"]),
        helper.make_node("Expand", ["column", "shape"], ["features"]),
    ]
    initializers = [
        helper.make_tensor("axes", TensorProto.INT64, [3], [1, 2, 3]),
        helper.make_tensor("one", TensorProto.INT64, [1], [1]),
        helper.make_tensor("shape", TensorProto.INT64, [2], [1, total]),
    ]
    graph = helper.make_graph(
        nodes,
        "tiny_backbone",
        [helper.make_tensor_value_info("pixel_values", TensorProto.FLOAT, ["batch", 3, HEIGHT, WIDTH])],
        [helper.make_tensor_value_info("features", TensorProto.FLOAT, ["batch", total])],
        initializers,
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", OPSET)])
    model.ir_version = 9
    metadata = {
        "ttrally.backbone": "tiny-test-model",
        "ttrally.input_height": str(HEIGHT),
        "ttrally.input_width": str(WIDTH),
        "ttrally.mean": json.dumps(IMAGENET_MEAN),
        "ttrally.std": json.dumps(IMAGENET_STD),
        "ttrally.parts": json.dumps(FEATURE_PARTS),
        "ttrally.part_dims": str(PART_DIMS),
    }
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key, entry.value = key, value
    onnx.checker.check_model(model)
    return model


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(build(), str(args.out))
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()

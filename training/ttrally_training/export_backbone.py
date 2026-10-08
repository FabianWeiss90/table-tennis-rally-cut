# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Exports the DINOv2 image backbone to ONNX for `ttrally features`.

Downloads the pretrained weights from Hugging Face (Apache-2.0), wraps them with the pooling of
backbone.py, exports to ONNX with the input size and normalisation stored as model metadata, and
checks the exported model numerically against PyTorch.

With --precision fp16 the weights and computations are converted to 16-bit floats, which GPUs
run faster; inputs and outputs stay 32-bit, so `ttrally features` uses both variants the same way.
Features of the two variants differ slightly, so use one variant consistently for training and
detection.

Usage (from the training directory):
    uv run python -m ttrally_training.export_backbone --out ../data/models/dinov2-vitb14.onnx
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime
import torch
from onnxruntime.transformers.float16 import DEFAULT_OP_BLOCK_LIST, convert_float_to_float16

from ttrally_training.backbone import (
    FEATURE_PARTS,
    IMAGENET_MEAN,
    IMAGENET_STD,
    PATCH_SIZE,
    PooledBackbone,
    load_dinov2,
)

OPSET = 18
# Largest accepted difference between ONNX and PyTorch, relative to the largest feature value
TOLERANCE = {"fp32": 1e-3, "fp16": 2e-2}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--model", default="facebook/dinov2-base", help="Hugging Face model id")
    parser.add_argument("--height", type=int, default=224, help="input height (multiple of 14)")
    parser.add_argument("--width", type=int, default=392, help="input width (multiple of 14)")
    parser.add_argument("--precision", choices=sorted(TOLERANCE), default="fp32",
                        help="number format of weights and computations (fp16 is faster on GPUs)")
    parser.add_argument("--out", type=Path, required=True, help="ONNX file to write")
    return parser.parse_args()


def export(module: torch.nn.Module, height: int, width: int, out: Path) -> None:
    example = torch.randn(2, 3, height, width)
    out.parent.mkdir(parents=True, exist_ok=True)
    program = torch.onnx.export(
        module,
        (example,),
        input_names=["pixel_values"],
        output_names=["features"],
        dynamic_shapes={"pixel_values": {0: torch.export.Dim("batch")}},
        opset_version=OPSET,
        dynamo=True,
        verbose=False,
    )
    program.save(str(out))


def to_fp16(out: Path) -> None:
    """Converts weights and computations to float16, keeping float32 inputs and outputs."""
    # Resize (interpolation of the position embeddings to the input size) has no valid float16
    # form for bicubic scaling in ONNX Runtime; it runs once per image and stays float32.
    model = convert_float_to_float16(
        onnx.load(str(out)),
        keep_io_types=True,
        op_block_list=[*DEFAULT_OP_BLOCK_LIST, "Resize"],
    )
    onnx.save(model, str(out))


def add_metadata(
    out: Path, model_id: str, precision: str, height: int, width: int, dims: int
) -> None:
    model = onnx.load(str(out))
    metadata = {
        # The precision is part of the name, so the feature manifests tell the variants apart
        "ttrally.backbone": model_id if precision == "fp32" else f"{model_id} ({precision})",
        "ttrally.precision": precision,
        "ttrally.input_height": str(height),
        "ttrally.input_width": str(width),
        "ttrally.mean": json.dumps(IMAGENET_MEAN),
        "ttrally.std": json.dumps(IMAGENET_STD),
        "ttrally.parts": json.dumps(FEATURE_PARTS),
        "ttrally.part_dims": str(dims),
    }
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key, entry.value = key, value
    onnx.save(model, str(out))


def verify(module: torch.nn.Module, height: int, width: int, out: Path) -> float:
    images = torch.randn(3, 3, height, width)
    with torch.no_grad():
        expected = module(images).numpy()
    session = onnxruntime.InferenceSession(str(out), providers=["CPUExecutionProvider"])
    actual = session.run(None, {"pixel_values": images.numpy()})[0]
    return float(np.max(np.abs(actual - expected)) / max(1.0, float(np.max(np.abs(expected)))))


def main() -> int:
    args = parse_args()
    if args.height % PATCH_SIZE or args.width % PATCH_SIZE:
        print(f"height and width must be multiples of {PATCH_SIZE}", file=sys.stderr)
        return 2

    print(f"Loading {args.model} ...")
    dinov2 = load_dinov2(args.model)
    module = PooledBackbone(dinov2, args.height, args.width).eval()
    dims = dinov2.config.hidden_size

    print(f"Exporting to {args.out} ({args.height}x{args.width}, {args.precision}, opset {OPSET}) ...")
    with torch.no_grad():
        export(module, args.height, args.width, args.out)
    if args.precision == "fp16":
        to_fp16(args.out)
    add_metadata(args.out, args.model, args.precision, args.height, args.width, dims)

    error = verify(module, args.height, args.width, args.out)
    print(f"Relative difference ONNX vs. PyTorch: {error:.2e}")
    if error > TOLERANCE[args.precision]:
        print("Export check failed", file=sys.stderr)
        return 1
    digest = hashlib.sha256(args.out.read_bytes()).hexdigest()[:16]
    print(f"OK: {len(FEATURE_PARTS)} x {dims} = {len(FEATURE_PARTS) * dims} values per image, "
          f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

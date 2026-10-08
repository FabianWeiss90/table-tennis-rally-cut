# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""ONNX export of the trained rally detector for `ttrally detect`.

The model takes the features of one video as `ttrally features` writes them, (1, rows, dims)
float32, and returns the calibrated rally probability of every row, (1, rows). Everything else
`ttrally detect` needs is stored as metadata: which backbone variant and feature settings the
features must come from, and how to decode the probabilities into rallies.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime
import torch

from ttrally_training.model import RallyProbability
from ttrally_training.training import TrainedModel

OPSET = 18
FORMAT_VERSION = "1"
TOLERANCE = 1e-4  # largest accepted difference of probabilities, ONNX vs. PyTorch
EXAMPLE_ROWS = 600
CHECK_ROWS = 937  # a different length than the example, to check the dynamic axis


def detector_metadata(manifest: dict, decoding: dict, training: dict) -> dict[str, str]:
    """Metadata keys read by `ttrally detect`; feature settings come from a features manifest
    of the training videos."""
    return {
        "ttrally.kind": "rally-detector",
        "ttrally.format_version": FORMAT_VERSION,
        "ttrally.backbone": manifest["model"],
        "ttrally.backbone_execution_provider": manifest.get("execution_provider", ""),
        "ttrally.parts": json.dumps(manifest["parts"]),
        "ttrally.part_dims": str(manifest["part_dims"]),
        "ttrally.input_size": json.dumps(manifest["input_size"]),
        "ttrally.sample_rate_hz": str(manifest["sample_rate_hz"]),
        "ttrally.decoding": json.dumps(decoding),
        "ttrally.training": json.dumps(training),
    }


def export(trained: TrainedModel, dims: int, metadata: dict[str, str], out: Path) -> float:
    """Writes the ONNX model and returns its largest difference to PyTorch on random input."""
    wrapper = RallyProbability(trained.model.cpu(), trained.temperature).eval()
    out.parent.mkdir(parents=True, exist_ok=True)
    example = torch.randn(1, EXAMPLE_ROWS, dims)
    with torch.no_grad():
        program = torch.onnx.export(
            wrapper, (example,),
            input_names=["features"], output_names=["rally_probability"],
            dynamic_shapes={"features": {1: torch.export.Dim("rows")}},
            opset_version=OPSET, dynamo=True, verbose=False)
    program.save(str(out))
    model = onnx.load(str(out))
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key, entry.value = key, value
    onnx.save(model, str(out))
    return verify(wrapper, dims, out)


def verify(wrapper: RallyProbability, dims: int, out: Path) -> float:
    features = torch.randn(1, CHECK_ROWS, dims)
    with torch.no_grad():
        expected = wrapper(features).numpy()
    session = onnxruntime.InferenceSession(str(out), providers=["CPUExecutionProvider"])
    actual = session.run(None, {"features": features.numpy()})[0]
    return float(np.max(np.abs(actual - expected)))

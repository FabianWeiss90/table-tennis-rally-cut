# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Writes the fixtures that keep `ttrally detect` (C++) in line with the training code (Python).

- tiny_detector.onnx: a small untrained rally detector with the interface and metadata of the
  exported one, for features of the tiny test backbone (make_test_model.py), plus an input and
  the probabilities PyTorch computes for it.
- decoding_*: rally probabilities, and for several decoding parameters the rallies decode()
  finds and how evaluate() scores them against fixed annotated rallies. The C++ port must
  reproduce them exactly.

Usage (from the training directory):
    uv run python -m ttrally_training.make_detection_fixtures --out ../tests/fixtures
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np
import torch

from ttrally_training.backbone import FEATURE_PARTS
from ttrally_training.decoding import Segment, decode
from ttrally_training.export_detector import detector_metadata, export
from ttrally_training.make_test_model import HEIGHT, PART_DIMS, WIDTH
from ttrally_training.metrics import evaluate
from ttrally_training.model import ModelConfig, RallyMSTCN
from ttrally_training.training import TrainedModel

RATE = 10.0
ROWS = 400
SEED = 7
DETECTOR_ROWS = 57
DECODING_CASES = [
    {"method": "threshold", "threshold": 0.5, "merge_gap_s": 0.0, "min_rally_s": 0.0},
    {"method": "threshold", "threshold": 0.45, "merge_gap_s": 1.0, "min_rally_s": 2.0},
    {"method": "threshold", "threshold": 0.7, "merge_gap_s": 3.0, "min_rally_s": 0.5},
    {"method": "viterbi", "mean_rally_s": 6.0, "mean_pause_s": 20.0},
    {"method": "viterbi", "mean_rally_s": 30.0, "mean_pause_s": 2.0},
]
ANNOTATED = [Segment(40, 99), Segment(150, 189), Segment(260, 330)]


def write_detector(out: Path) -> None:
    torch.manual_seed(SEED)
    dims = len(FEATURE_PARTS) * PART_DIMS
    model = RallyMSTCN(dims, ModelConfig(hidden=8, levels=3, stages=2)).eval()
    trained = TrainedModel(model, temperature=1.5)
    manifest = {"model": "tiny-test-model", "execution_provider": "cpu",
                "parts": list(FEATURE_PARTS), "part_dims": PART_DIMS,
                "input_size": [WIDTH, HEIGHT], "sample_rate_hz": RATE}
    decoding = {"method": "threshold", "threshold": 0.5, "merge_gap_s": 1.0, "min_rally_s": 0.5}
    export(trained, dims, detector_metadata(manifest, decoding, {"videos": []}),
           out / "tiny_detector.onnx")
    features = np.random.default_rng(SEED).normal(size=(DETECTOR_ROWS, dims)).astype(np.float32)
    np.save(out / "tiny_detector_input.npy", features)
    with torch.no_grad():
        probabilities = trained.predict(features, torch.device("cpu")).astype(np.float32)
    np.save(out / "tiny_detector_output.npy", probabilities)


def rally_like_probabilities() -> np.ndarray:
    """Noisy plateaus with a dip inside a rally and short spikes in the pauses, so that the
    decoding parameters lead to different rallies (merged, dropped, split)."""
    rng = np.random.default_rng(SEED)
    truth = np.zeros(ROWS)
    for segment in ANNOTATED:
        truth[segment.start : segment.end + 1] = 1.0
    probabilities = 0.2 + 0.6 * truth + rng.normal(0, 0.22, ROWS)
    probabilities[66:72] = 0.1  # a dip of 0.6 s inside the first rally
    probabilities[120:122] = 0.95  # a spike of 0.2 s in a pause
    probabilities[205:214] = 0.75  # a short false rally of 0.9 s
    return np.clip(probabilities, 0, 1).astype(np.float32)


def write_decoding(out: Path) -> None:
    probabilities = rally_like_probabilities()
    np.save(out / "decoding_probabilities.npy", probabilities)
    with (out / "decoding_expected.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file, lineterminator="\n")
        writer.writerow(["case", "params", "first_row", "last_row", "f1", "boundary_mae_s",
                         "row_f1"])
        for case, params in enumerate(DECODING_CASES):
            segments = decode(probabilities.astype(np.float64), RATE, params)
            metrics = evaluate(segments, ANNOTATED, ROWS, RATE)
            for segment in segments or [Segment(-1, -1)]:
                writer.writerow([case, json.dumps(params), segment.start, segment.end,
                                 repr(metrics.f1), repr(metrics.boundary_mae_s),
                                 repr(metrics.row_f1)])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    write_detector(args.out)
    write_decoding(args.out)
    print(f"Wrote detection fixtures to {args.out}")


if __name__ == "__main__":
    main()

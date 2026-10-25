# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Trains the rally detector on the chosen videos, validates it on the others and exports it.

1. Loads the videos that have labels, features and a complete review in `annotate`.
2. Trains on the chosen videos (--videos; default: all, then without validation).
3. Validation on all other loaded videos: the decoding parameters are chosen on their
   predictions, then segment F1 and boundary error are measured per video.
4. Exports the model with the decoding parameters and a report.

Usage (from the training directory):
    uv run python -m ttrally_training.train --library /videos/table-tennis \\
        --videos "2026-10-03-Club/match-a" "2026-10-10-Away/match-b"
    uv run python -m ttrally_training.train --features-dir ../data/features \\
        --annotations-dir ../data/annotations --videos match-a  # without a library
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import asdict, replace
from pathlib import Path

import torch

from ttrally_training.data import (
    WORK_FOLDER,
    DataError,
    Selection,
    Video,
    select_library_videos,
    select_videos,
)
from ttrally_training.decoding import DEFAULT_PARAMS
from ttrally_training.export_detector import TOLERANCE, detector_metadata, export
from ttrally_training.metrics import mean_metrics
from ttrally_training.model import ModelConfig
from ttrally_training.training import TrainConfig, train, validate

DEFAULT_SEED = 42


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--library", type=Path,
                        help="folder with one folder per match day, each with ttrally-data/")
    parser.add_argument("--features-dir", type=Path, default=Path("../data/features"),
                        help="without --library")
    parser.add_argument("--annotations-dir", type=Path, default=Path("../data/annotations"),
                        help="without --library")
    parser.add_argument("--videos", nargs="+",
                        help="training videos: keys <match day>/<video id> with --library, else "
                             "video ids (default: all, no validation); all other completely "
                             "annotated videos are validated")
    parser.add_argument("--out", type=Path,
                        help="default: <library>/ttrally-data/models/rally-detector.onnx, "
                             "without --library ../weights/rally-detector.onnx")
    parser.add_argument("--report", type=Path, help="default: <out>.report.json")
    parser.add_argument("--epochs", type=int, default=TrainConfig.epochs)
    parser.add_argument("--hidden", type=int, default=ModelConfig.hidden)
    parser.add_argument("--levels", type=int, default=ModelConfig.levels)
    parser.add_argument("--stages", type=int, default=ModelConfig.stages)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto",
                        help="cuda also covers ROCm builds of PyTorch")
    return parser.parse_args()


def choose_device(name: str) -> torch.device:
    if name == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(name)


def describe(videos: list[Video]) -> None:
    for video in videos:
        minutes = len(video.labels) / video.sample_rate_hz / 60
        ignored = (1 - video.mask).sum() / video.sample_rate_hz / 60
        print(f"  {video.video_id}: {minutes:.1f} min, {len(video.rallies)} rallies, "
              f"{video.labels.mean():.0%} of the time in rallies"
              + (f", {ignored:.1f} min ignored" if ignored > 0 else ""))


def split(selection: Selection, chosen: list[str] | None) -> tuple[list[Video], list[Video]]:
    """Training videos (the chosen ones, default all) and validation videos (the others)."""
    if chosen is None:
        return selection.videos, []
    loaded = {v.video_id for v in selection.videos}
    for name in chosen:
        if name not in loaded:
            reason = selection.skipped.get(name, "no labels found")
            raise DataError(f"training video {name} cannot be used: {reason}")
    return ([v for v in selection.videos if v.video_id in chosen],
            [v for v in selection.videos if v.video_id not in chosen])


def print_metrics(name: str, metrics: dict) -> None:
    print(f"  {name:<28} Seg-F1 {metrics['f1']:.3f}  P {metrics['precision']:.3f}  "
          f"R {metrics['recall']:.3f}  boundaries {metrics['boundary_mae_s']:.2f} s  "
          f"({metrics['matched']}/{metrics['annotated']} rallies found, "
          f"{metrics['predicted']} predicted)")


def video_summary(videos: list[Video]) -> dict:
    return {v.video_id: {"rallies": len(v.rallies), "rows": len(v.labels),
                         "ignored_rows": int((1 - v.mask).sum())} for v in videos}


def main() -> int:
    args = parse_args()
    device = choose_device(args.device)
    config = TrainConfig(epochs=args.epochs,
                         model=replace(ModelConfig(), hidden=args.hidden, levels=args.levels,
                                       stages=args.stages))
    try:
        if args.library:
            selection = select_library_videos(args.library)
        else:
            selection = select_videos(args.features_dir, args.annotations_dir)
        training, validation = split(selection, args.videos)
    except DataError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2
    if not training:
        print("Error: no usable video (labels, features and a complete review are needed)",
              file=sys.stderr)
        return 2
    print(f"Training videos ({len(training)}):")
    describe(training)
    print(f"Validation videos ({len(validation)}):")
    describe(validation)
    for name, reason in selection.skipped.items():
        print(f"  {name}: not used ({reason})")
    out = args.out or (args.library / WORK_FOLDER / "models" / "rally-detector.onnx"
                       if args.library else Path("../weights/rally-detector.onnx"))
    out.parent.mkdir(parents=True, exist_ok=True)
    manifest = training[0].manifest
    print(f"Features: {manifest['model']} on {manifest.get('execution_provider', '?')}, "
          f"{manifest['sample_rate_hz']} per second; device {device}")
    started = time.monotonic()

    print("Training:")
    trained = train(training, config, args.seed, device)
    report: dict = {"training": video_summary(training), "validation": video_summary(validation),
                    "skipped": selection.skipped, "config": asdict(config), "seed": args.seed}
    decoding = DEFAULT_PARAMS
    if validation:
        print("Validation (decoding chosen on these videos):")
        result = validate(trained, validation, device)
        decoding = result.decoding
        for name, metrics in result.per_video.items():
            print_metrics(name, metrics.to_dict())
        mean = mean_metrics(list(result.per_video.values()))
        print_metrics("mean over validation videos", mean)
        report["evaluation"] = {"per_video": {k: m.to_dict() for k, m in result.per_video.items()},
                                "mean": mean}
    else:
        print("No validation videos; default decoding.")
    print(f"Decoding: {decoding}")

    training_info = {"videos": [v.video_id for v in training],
                     "validation_videos": [v.video_id for v in validation],
                     "rallies": sum(len(v.rallies) for v in training),
                     "temperature": trained.temperature,
                     "evaluation": report.get("evaluation", {}).get("mean")}
    error = export(trained, training[0].features.shape[1],
                   detector_metadata(manifest, decoding, training_info), out)
    report.update(decoding=decoding, temperature=trained.temperature, export_check=error,
                  minutes=(time.monotonic() - started) / 60)
    report_path = args.report or out.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Model: {out} (ONNX vs. PyTorch: {error:.1e}); report: {report_path}")
    if error > TOLERANCE:
        print("Export check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Trains the rally detector on all completely annotated videos and exports it to ONNX.

1. Selects the videos that have labels, features and a complete review in `annotate`.
2. Leave-one-group-out cross-validation (each video is its own group unless --groups says
   otherwise): segment F1 and boundary error per held-out video.
3. Trains the final model on all videos, with the decoding parameters that worked best on the
   held-out predictions, and exports it with a report.

Usage (from the training directory):
    uv run python -m ttrally_training.train
    uv run python -m ttrally_training.train --groups ../data/annotations/groups.csv --runs 3
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import asdict, replace
from pathlib import Path

import torch

from ttrally_training.data import DataError, Video, load_groups, select_videos
from ttrally_training.decoding import DEFAULT_PARAMS
from ttrally_training.export_detector import TOLERANCE, detector_metadata, export
from ttrally_training.metrics import mean_metrics
from ttrally_training.model import ModelConfig
from ttrally_training.training import (
    TrainConfig,
    best_params,
    cross_validate,
    evaluate_held_out,
    train,
)

DEFAULT_SEED = 42


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--features-dir", type=Path, default=Path("../data/features"))
    parser.add_argument("--annotations-dir", type=Path, default=Path("../data/annotations"))
    parser.add_argument("--videos", nargs="+", help="video ids (default: all annotated)")
    parser.add_argument("--groups", type=Path,
                        help="CSV video_id,group: videos of one group are held out together")
    parser.add_argument("--out", type=Path, default=Path("../weights/rally-detector.onnx"))
    parser.add_argument("--report", type=Path, help="default: <out>.report.json")
    parser.add_argument("--epochs", type=int, default=TrainConfig.epochs)
    parser.add_argument("--hidden", type=int, default=ModelConfig.hidden)
    parser.add_argument("--levels", type=int, default=ModelConfig.levels)
    parser.add_argument("--stages", type=int, default=ModelConfig.stages)
    parser.add_argument("--runs", type=int, default=1,
                        help="cross-validation runs with different seeds, averaged")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto",
                        help="cuda also covers ROCm builds of PyTorch")
    parser.add_argument("--skip-evaluation", action="store_true",
                        help="train the final model only, with default decoding parameters")
    return parser.parse_args()


def choose_device(name: str) -> torch.device:
    if name == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(name)


def describe(videos: list[Video], skipped: dict[str, str]) -> None:
    for video in videos:
        minutes = len(video.labels) / video.sample_rate_hz / 60
        ignored = (1 - video.mask).sum() / video.sample_rate_hz / 60
        print(f"  {video.video_id}: {minutes:.1f} min, {len(video.rallies)} rallies, "
              f"{video.labels.mean():.0%} of the time in rallies"
              + (f", {ignored:.1f} min ignored" if ignored > 0 else ""))
    for video_id, reason in skipped.items():
        print(f"  {video_id}: skipped ({reason})")


def print_metrics(name: str, metrics: dict) -> None:
    print(f"  {name:<28} Seg-F1 {metrics['f1']:.3f}  P {metrics['precision']:.3f}  "
          f"R {metrics['recall']:.3f}  boundaries {metrics['boundary_mae_s']:.2f} s  "
          f"({metrics['matched']}/{metrics['annotated']} rallies found, "
          f"{metrics['predicted']} predicted)")


def main() -> int:
    args = parse_args()
    device = choose_device(args.device)
    config = TrainConfig(epochs=args.epochs,
                         model=replace(ModelConfig(), hidden=args.hidden, levels=args.levels,
                                       stages=args.stages))
    try:
        selection = select_videos(args.features_dir, args.annotations_dir, args.videos)
    except DataError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2
    videos = selection.videos
    print(f"Videos ({len(videos)} usable):")
    describe(videos, selection.skipped)
    if not videos:
        print("Error: no usable video (labels, features and a complete review are needed)",
              file=sys.stderr)
        return 2
    groups = load_groups(args.groups, videos)
    manifest = videos[0].manifest
    print(f"Features: {manifest['model']} on {manifest.get('execution_provider', '?')}, "
          f"{manifest['sample_rate_hz']} per second; device {device}")
    started = time.monotonic()

    report: dict = {"videos": {v.video_id: {"rallies": len(v.rallies), "rows": len(v.labels),
                                            "ignored_rows": int((1 - v.mask).sum()),
                                            "group": groups[v.video_id]} for v in videos},
                    "skipped": selection.skipped, "config": asdict(config), "seed": args.seed}
    decoding = DEFAULT_PARAMS
    if args.skip_evaluation or len(set(groups.values())) < 2:
        print("Cross-validation skipped (needs videos of two groups); default decoding.")
    else:
        print("Cross-validation:")
        probabilities = cross_validate(videos, groups, config, args.seed, args.runs, device)
        evaluation = evaluate_held_out(videos, groups, probabilities)
        for video_id, metrics in evaluation.per_video.items():
            print_metrics(video_id, metrics.to_dict())
        mean = mean_metrics(list(evaluation.per_video.values()))
        print_metrics("mean over held-out videos", mean)
        decoding = best_params(videos, probabilities)
        report["evaluation"] = {
            "per_video": {k: m.to_dict() for k, m in evaluation.per_video.items()},
            "mean": mean, "decoding_per_group": evaluation.params_per_group}
    print(f"Decoding: {decoding}")

    print("Final model on all videos:")
    trained = train(videos, config, args.seed, device)
    training_info = {"videos": [v.video_id for v in videos],
                     "rallies": sum(len(v.rallies) for v in videos),
                     "temperature": trained.temperature,
                     "evaluation": report.get("evaluation", {}).get("mean")}
    error = export(trained, videos[0].features.shape[1],
                   detector_metadata(manifest, decoding, training_info), args.out)
    report.update(decoding=decoding, temperature=trained.temperature, export_check=error,
                  minutes=(time.monotonic() - started) / 60)
    report_path = args.report or args.out.with_suffix(".report.json")
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Model: {args.out} (ONNX vs. PyTorch: {error:.1e}); report: {report_path}")
    if error > TOLERANCE:
        print("Export check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

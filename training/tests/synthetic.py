# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Synthetic training data in the formats of `ttrally features` and `ttrally annotate`.

Rows inside a rally get features with a shifted mean, so a model can learn to find them.
"""

from __future__ import annotations

import csv
import json
from pathlib import Path

import numpy as np

FPS = 60.0
RATE = 10.0
DIMS = 8
FRAMES_PER_ROW = int(FPS / RATE)


def manifest(video_id: str, rows: int, model: str = "fake-backbone (fp16)",
             provider: str = "webgpu") -> dict:
    return {"format": "ttrally-features-1", "video_id": video_id, "video_path": f"{video_id}.mp4",
            "video_fps": [60, 1], "video_frame_count": rows * FRAMES_PER_ROW,
            "sample_rate_hz": RATE, "rows": rows, "dims": DIMS, "parts": ["cls", "mean"],
            "part_dims": DIMS // 2, "model": model, "execution_provider": provider,
            "input_size": [392, 224], "fingerprint": "0"}


def rallies_for(rows: int, rng: np.random.Generator) -> list[tuple[int, int]]:
    """Rallies of 4-12 s separated by pauses of 8-20 s, in frames."""
    rallies, row = [], int(rng.integers(30, 80))
    while True:
        length = int(rng.integers(40, 120))
        if row + length >= rows:
            return rallies
        rallies.append((row * FRAMES_PER_ROW, (row + length) * FRAMES_PER_ROW - 1))
        row += length + int(rng.integers(80, 200))


def write_video(root: Path, video_id: str, rows: int, seed: int, review: str = "complete",
                **manifest_fields) -> list[tuple[int, int]]:
    """Writes features under root/features and labels plus review under root/annotations.

    review: "complete", "incomplete" or "missing".
    """
    rng = np.random.default_rng(seed)
    rallies = rallies_for(rows, rng)
    frames = np.arange(rows, dtype=np.int64) * FRAMES_PER_ROW
    in_rally = np.zeros(rows, dtype=bool)
    for start, end in rallies:
        in_rally |= (frames >= start) & (frames <= end)
    features = rng.normal(0.0, 1.0, (rows, DIMS)).astype(np.float32)
    features[in_rally, :3] += 2.5

    directory = root / "features" / video_id
    directory.mkdir(parents=True)
    np.save(directory / "features.npy", features)
    np.save(directory / "times.npy", np.arange(rows) / RATE)
    np.save(directory / "frames.npy", frames)
    (directory / "manifest.json").write_text(
        json.dumps(manifest(video_id, rows, **manifest_fields)), encoding="utf-8")

    annotations = root / "annotations"
    annotations.mkdir(exist_ok=True)
    with (annotations / f"{video_id}.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["video_id", "rally_id", "start_frame", "end_frame", "fps",
                         "serve_contact_frame", "flags", "notes"])
        for number, (start, end) in enumerate(rallies, start=1):
            flags = "let" if number == 2 else ("aborted_toss" if number == 3 else "")
            writer.writerow([video_id, number, start, end, "60", "", flags, ""])
    if review != "missing":
        with (annotations / f"{video_id}.review.csv").open("w", newline="",
                                                           encoding="utf-8") as file:
            writer = csv.writer(file)
            writer.writerow(["kind", "id", "first_frame", "last_frame", "status"])
            writer.writerow(["gap", 1, 0, 100, "reviewed"])
            writer.writerow(["candidate", 1, 101, 500,
                             "annotated" if review == "complete" else "open"])
    return rallies

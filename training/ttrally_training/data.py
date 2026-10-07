# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Training data: features written by `ttrally features`, labels and review progress of
`ttrally annotate`.

A video is used for training only if its review is complete (every candidate and gap done),
because an unchecked gap may hide a missed rally that would be learned as "no rally".

Each feature row belongs to one frame of the original (frames.npy), so a row is labelled
"rally" if that frame lies inside an annotated rally. All rallies count, including those flagged
aborted_toss or let.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np

# Manifest fields that must agree between all videos of one training run
CONSISTENT_FIELDS = ("model", "execution_provider", "parts", "part_dims", "sample_rate_hz",
                     "input_size")


class DataError(Exception):
    """Training data is missing, malformed or inconsistent."""


@dataclass(frozen=True)
class Rally:
    """Annotated rally in frames of the original, end inclusive."""

    start_frame: int
    end_frame: int


@dataclass(frozen=True)
class ReviewSummary:
    items: int
    open_items: int

    @property
    def complete(self) -> bool:
        return self.items > 0 and self.open_items == 0


@dataclass
class Video:
    """One video ready for training: feature rows and their labels."""

    video_id: str
    features: np.ndarray  # (rows, dims) float32
    labels: np.ndarray  # (rows,) float32, 1 = rally
    times_s: np.ndarray  # (rows,) time of each row
    manifest: dict
    rallies: list[Rally]

    @property
    def sample_rate_hz(self) -> float:
        return float(self.manifest["sample_rate_hz"])


def load_rallies(annotations_dir: Path, video_id: str) -> list[Rally]:
    """Rallies of annotations/<video_id>.csv (all flags count as rally)."""
    path = annotations_dir / f"{video_id}.csv"
    if not path.exists():
        raise DataError(f"{path} does not exist")
    with path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    rallies = []
    for line, row in enumerate(rows, start=2):
        try:
            rallies.append(Rally(int(row["start_frame"]), int(row["end_frame"])))
        except (KeyError, ValueError) as error:
            raise DataError(f"{path}:{line}: invalid rally ({error})") from None
    return rallies


def load_review(annotations_dir: Path, video_id: str) -> ReviewSummary:
    """Review progress of annotations/<video_id>.review.csv; no file means nothing reviewed."""
    path = annotations_dir / f"{video_id}.review.csv"
    if not path.exists():
        return ReviewSummary(items=0, open_items=0)
    with path.open(newline="", encoding="utf-8") as file:
        statuses = [row["status"] for row in csv.DictReader(file)]
    return ReviewSummary(items=len(statuses), open_items=statuses.count("open"))


def row_labels(frames: np.ndarray, rallies: list[Rally]) -> np.ndarray:
    """1 for every row whose frame lies inside a rally, else 0."""
    labels = np.zeros(len(frames), dtype=np.float32)
    for rally in rallies:
        labels[(frames >= rally.start_frame) & (frames <= rally.end_frame)] = 1.0
    return labels


def load_features(features_dir: Path, video_id: str) -> tuple[np.ndarray, np.ndarray,
                                                               np.ndarray, dict]:
    """Feature rows, row times, row frames and manifest of <features_dir>/<video_id>/."""
    directory = features_dir / video_id
    try:
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
        features = np.load(directory / "features.npy").astype(np.float32, copy=False)
        times = np.load(directory / "times.npy")
        frames = np.load(directory / "frames.npy")
    except FileNotFoundError as error:
        raise DataError(f"features of {video_id} are incomplete: {error.filename}") from None
    if not (len(features) == len(times) == len(frames) == manifest.get("rows")):
        raise DataError(f"features of {video_id}: row counts do not match the manifest")
    return features, times, frames, manifest


def load_video(features_dir: Path, annotations_dir: Path, video_id: str) -> Video:
    features, times, frames, manifest = load_features(features_dir, video_id)
    rallies = load_rallies(annotations_dir, video_id)
    return Video(video_id, features, row_labels(frames, rallies), times, manifest, rallies)


@dataclass
class Selection:
    videos: list[Video]
    skipped: dict[str, str]  # video id -> reason


def select_videos(features_dir: Path, annotations_dir: Path,
                  video_ids: list[str] | None = None) -> Selection:
    """Loads all videos with labels, features and a complete review (or the given ones).

    Videos lacking one of these are skipped with a reason. Raises DataError if the features of
    the selected videos were computed differently (model, execution provider, sampling, ...).
    """
    if video_ids is None:
        video_ids = sorted(p.name.removesuffix(".csv") for p in annotations_dir.glob("*.csv")
                           if not p.name.endswith(".review.csv"))
    videos: list[Video] = []
    skipped: dict[str, str] = {}
    for video_id in video_ids:
        review = load_review(annotations_dir, video_id)
        if not review.complete:
            skipped[video_id] = (
                "review not started" if review.items == 0
                else f"review incomplete ({review.open_items} of {review.items} items open)")
            continue
        if not (features_dir / video_id / "manifest.json").exists():
            skipped[video_id] = f"no features in {features_dir / video_id}"
            continue
        videos.append(load_video(features_dir, annotations_dir, video_id))
    check_consistent(videos)
    return Selection(videos, skipped)


def check_consistent(videos: list[Video]) -> None:
    """All feature sets must come from the same model variant and settings."""
    if not videos:
        return
    reference = videos[0]
    for video in videos[1:]:
        for field in CONSISTENT_FIELDS:
            if video.manifest.get(field) != reference.manifest.get(field):
                raise DataError(
                    f"features of {video.video_id} and {reference.video_id} differ in {field}: "
                    f"{video.manifest.get(field)!r} vs. {reference.manifest.get(field)!r}; "
                    "recompute them with the same model and options")


def load_groups(path: Path | None, videos: list[Video]) -> dict[str, str]:
    """Group of each video for the cross-validation (CSV video_id,group, e.g. one group per
    YouTube channel); videos not listed form a group of their own."""
    groups = {video.video_id: video.video_id for video in videos}
    if path is None:
        return groups
    with path.open(newline="", encoding="utf-8") as file:
        for row in csv.DictReader(file):
            if row["video_id"] in groups:
                groups[row["video_id"]] = row["group"]
    return groups

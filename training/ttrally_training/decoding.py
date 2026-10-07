# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original files: src/fusion/combine.py (scores_to_segments), src/supervised/common.py
#   (viterbi_decode, viterbi_to_rallies)
# Changes: all parameters in seconds instead of frames at 120 fps (Viterbi: mean rally and pause
#   durations instead of per-frame transition probabilities); vectorised; parameters
#   serialisable, since `ttrally detect` has to decode exactly the same way.
"""From per-row rally probabilities to rally segments.

Two decoders: a threshold with merging of short interruptions and removal of short segments,
and a two-state hidden Markov model decoded with Viterbi. Both are described by a dictionary
(`method` plus parameters in seconds) that is stored in the exported model.
"""

from __future__ import annotations

import itertools
import math
from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class Segment:
    """Rally in rows of the feature grid, end inclusive."""

    start: int
    end: int

    @property
    def length(self) -> int:
        return self.end - self.start + 1

    def iou(self, other: Segment) -> float:
        intersection = min(self.end, other.end) - max(self.start, other.start) + 1
        if intersection <= 0:
            return 0.0
        return intersection / (self.length + other.length - intersection)


def segments_of(mask: np.ndarray) -> list[Segment]:
    """Runs of True (or 1) in a mask."""
    padded = np.concatenate([[0], np.asarray(mask, dtype=np.int8), [0]])
    changes = np.flatnonzero(np.diff(padded))
    return [Segment(int(s), int(e) - 1) for s, e in zip(changes[::2], changes[1::2])]


def threshold_decode(proba: np.ndarray, sample_rate_hz: float, threshold: float,
                     merge_gap_s: float, min_rally_s: float) -> list[Segment]:
    """Rows above the threshold; segments separated by at most merge_gap_s are merged, then
    segments shorter than min_rally_s are dropped."""
    segments = segments_of(proba >= threshold)
    merge_gap = merge_gap_s * sample_rate_hz
    merged: list[Segment] = []
    for segment in segments:
        if merged and segment.start - merged[-1].end <= merge_gap:
            merged[-1] = Segment(merged[-1].start, segment.end)
        else:
            merged.append(segment)
    min_length = min_rally_s * sample_rate_hz
    return [s for s in merged if s.length >= min_length]


def viterbi_decode(proba: np.ndarray, sample_rate_hz: float, mean_rally_s: float,
                   mean_pause_s: float) -> list[Segment]:
    """Most likely rally / pause sequence of a two-state HMM whose states last mean_rally_s and
    mean_pause_s on average; the probabilities are the emissions."""
    if len(proba) == 0:
        return []
    leave_rally = min(1.0, 1.0 / (mean_rally_s * sample_rate_hz))
    leave_pause = min(1.0, 1.0 / (mean_pause_s * sample_rate_hz))
    log = lambda value: math.log(max(value, 1e-300))  # noqa: E731
    stay_pause, start_rally = log(1 - leave_pause), log(leave_pause)
    end_rally, stay_rally = log(leave_rally), log(1 - leave_rally)

    p = np.clip(proba.astype(np.float64), 1e-7, 1 - 1e-7)
    emit_rally, emit_pause = np.log(p), np.log(1 - p)
    rows = len(p)
    came_from = np.empty((rows, 2), dtype=np.int8)  # previous state of state 0 / 1
    pause, rally = -math.log(2) + emit_pause[0], -math.log(2) + emit_rally[0]
    for t in range(1, rows):
        from_pause, from_rally = pause + stay_pause, rally + end_rally
        came_from[t, 0] = 0 if from_pause >= from_rally else 1
        new_pause = max(from_pause, from_rally) + emit_pause[t]
        to_rally_from_pause, to_rally_from_rally = pause + start_rally, rally + stay_rally
        came_from[t, 1] = 1 if to_rally_from_rally >= to_rally_from_pause else 0
        new_rally = max(to_rally_from_pause, to_rally_from_rally) + emit_rally[t]
        pause, rally = new_pause, new_rally

    states = np.empty(rows, dtype=np.int8)
    states[-1] = 0 if pause >= rally else 1
    for t in range(rows - 1, 0, -1):
        states[t - 1] = came_from[t, states[t]]
    return segments_of(states)


def decode(proba: np.ndarray, sample_rate_hz: float, params: dict) -> list[Segment]:
    """Decodes with parameters such as {"method": "threshold", "threshold": 0.5, ...}."""
    arguments = {key: value for key, value in params.items() if key != "method"}
    if params["method"] == "threshold":
        return threshold_decode(proba, sample_rate_hz, **arguments)
    if params["method"] == "viterbi":
        return viterbi_decode(proba, sample_rate_hz, **arguments)
    raise ValueError(f"unknown decoding method {params['method']!r}")


DEFAULT_PARAMS = {"method": "threshold", "threshold": 0.5, "merge_gap_s": 1.0,
                  "min_rally_s": 0.5}


def candidate_params() -> list[dict]:
    """Decoding parameters tried when tuning on held-out predictions. Short minimum durations
    are included because aborted tosses count as (very short) rallies."""
    candidates = [
        {"method": "threshold", "threshold": round(float(t), 2), "merge_gap_s": gap,
         "min_rally_s": length}
        for t, gap, length in itertools.product(np.arange(0.2, 0.91, 0.05),
                                                [0.0, 0.5, 1.0, 2.0, 3.0],
                                                [0.3, 0.5, 1.0, 1.5, 2.0])
    ]
    candidates += [
        {"method": "viterbi", "mean_rally_s": rally, "mean_pause_s": pause}
        for rally, pause in itertools.product([2.0, 4.0, 6.0, 10.0, 15.0],
                                              [5.0, 10.0, 20.0, 40.0, 80.0])
    ]
    return candidates

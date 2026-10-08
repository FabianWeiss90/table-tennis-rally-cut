# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original file: src/evaluation/metrics.py (SegmentMetrics, evaluate)
# Changes: boundary error in seconds; separate start and end errors; aggregation over videos.
"""Evaluation of predicted rally segments against the annotated ones.

Segment level: a prediction matches an unmatched annotated rally if their IoU reaches the
threshold (greedy, in prediction order); precision, recall and F1 count matches. For matched
pairs, the boundary error is the mean absolute difference of start and end in seconds. Row level:
precision, recall and F1 of the rally mask.

Ignored sections do not count: predictions lying mostly inside one are dropped, and their rows
are left out of the row-level metrics.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np

from ttrally_training.decoding import Segment

IOU_THRESHOLD = 0.5
MAX_IGNORED_SHARE = 0.5  # predictions with more of their rows ignored are dropped


@dataclass(frozen=True)
class SegmentMetrics:
    precision: float
    recall: float
    f1: float
    mean_iou: float  # over matched pairs
    boundary_mae_s: float  # mean of |start error| and |end error| over matched pairs
    start_mae_s: float
    end_mae_s: float
    row_precision: float
    row_recall: float
    row_f1: float
    annotated: int
    predicted: int
    matched: int

    def to_dict(self) -> dict:
        return asdict(self)


def _f1(precision: float, recall: float) -> float:
    return 2 * precision * recall / (precision + recall) if precision + recall > 0 else 0.0


def _mask(segments: list[Segment], rows: int) -> np.ndarray:
    mask = np.zeros(rows, dtype=bool)
    for segment in segments:
        mask[segment.start : segment.end + 1] = True
    return mask


def evaluate(predicted: list[Segment], annotated: list[Segment], rows: int,
             sample_rate_hz: float, iou_threshold: float = IOU_THRESHOLD,
             ignored: np.ndarray | None = None) -> SegmentMetrics:
    """`ignored`: boolean per row, True inside ignored sections."""
    counted = np.ones(rows, dtype=bool) if ignored is None else ~ignored
    predicted = [p for p in predicted
                 if 1.0 - counted[p.start : p.end + 1].mean() <= MAX_IGNORED_SHARE]
    matched: set[int] = set()
    ious: list[float] = []
    start_errors: list[float] = []
    end_errors: list[float] = []
    for prediction in predicted:
        best_iou, best = 0.0, -1
        for index, truth in enumerate(annotated):
            if index not in matched and (iou := prediction.iou(truth)) > best_iou:
                best_iou, best = iou, index
        if best >= 0 and best_iou >= iou_threshold:
            matched.add(best)
            ious.append(best_iou)
            start_errors.append(abs(prediction.start - annotated[best].start) / sample_rate_hz)
            end_errors.append(abs(prediction.end - annotated[best].end) / sample_rate_hz)

    precision = len(ious) / len(predicted) if predicted else 0.0
    recall = len(ious) / len(annotated) if annotated else 0.0
    mean = lambda values: float(np.mean(values)) if values else 0.0  # noqa: E731

    predicted_mask = _mask(predicted, rows) & counted
    annotated_mask = _mask(annotated, rows) & counted
    true_positive = int(np.sum(predicted_mask & annotated_mask))
    row_precision = true_positive / max(1, int(predicted_mask.sum()))
    row_recall = true_positive / max(1, int(annotated_mask.sum()))
    return SegmentMetrics(
        precision=precision, recall=recall, f1=_f1(precision, recall), mean_iou=mean(ious),
        boundary_mae_s=mean([(s + e) / 2 for s, e in zip(start_errors, end_errors)]),
        start_mae_s=mean(start_errors), end_mae_s=mean(end_errors),
        row_precision=row_precision, row_recall=row_recall,
        row_f1=_f1(row_precision, row_recall),
        annotated=len(annotated), predicted=len(predicted), matched=len(ious))


def mean_metrics(metrics: list[SegmentMetrics]) -> dict:
    """Per-video average of every metric (counts are summed)."""
    counts = ("annotated", "predicted", "matched")
    result = {}
    for field in SegmentMetrics.__dataclass_fields__:
        values = [getattr(m, field) for m in metrics]
        result[field] = int(sum(values)) if field in counts else float(np.mean(values))
    return result

# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import numpy as np

from ttrally_training.decoding import (
    Segment,
    candidate_params,
    decode,
    segments_of,
    threshold_decode,
    viterbi_decode,
)
from ttrally_training.metrics import evaluate, mean_metrics

RATE = 10.0


def test_segments_are_runs_of_the_mask():
    assert segments_of(np.array([0, 1, 1, 0, 1])) == [Segment(1, 2), Segment(4, 4)]
    assert segments_of(np.array([])) == []


def test_threshold_merges_short_interruptions_and_drops_short_segments():
    proba = np.zeros(100)
    proba[10:30] = 0.9
    proba[33:50] = 0.9  # 0.3 s interruption: merged
    proba[70:72] = 0.9  # 0.2 s blip: dropped
    segments = threshold_decode(proba, RATE, threshold=0.5, merge_gap_s=0.5, min_rally_s=0.5)
    assert segments == [Segment(10, 49)]


def test_viterbi_ignores_isolated_outliers():
    rng = np.random.default_rng(0)
    truth = np.zeros(600)
    truth[100:200] = 1
    truth[400:480] = 1
    proba = np.clip(truth * 0.7 + 0.15 + rng.normal(0, 0.1, 600), 0, 1)
    proba[300] = 0.99  # a single confident row in a pause
    segments = viterbi_decode(proba, RATE, mean_rally_s=8.0, mean_pause_s=20.0)
    assert len(segments) == 2
    assert abs(segments[0].start - 100) <= 2 and abs(segments[1].end - 479) <= 2


def test_decode_dispatches_on_the_method():
    proba = np.r_[np.zeros(20), np.ones(20), np.zeros(20)]
    for params in ({"method": "threshold", "threshold": 0.5, "merge_gap_s": 0.0,
                    "min_rally_s": 0.0},
                   {"method": "viterbi", "mean_rally_s": 2.0, "mean_pause_s": 2.0}):
        assert decode(proba, RATE, params) == [Segment(20, 39)]
    assert {p["method"] for p in candidate_params()} == {"threshold", "viterbi"}


def test_metrics_match_segments_and_measure_boundaries_in_seconds():
    annotated = [Segment(10, 59), Segment(100, 149), Segment(300, 309)]
    predicted = [Segment(12, 59), Segment(100, 155), Segment(200, 220)]
    metrics = evaluate(predicted, annotated, rows=400, sample_rate_hz=RATE)
    assert metrics.matched == 2
    assert metrics.precision == 2 / 3 and metrics.recall == 2 / 3
    assert metrics.start_mae_s == (0.2 + 0.0) / 2
    assert metrics.end_mae_s == (0.0 + 0.6) / 2
    perfect = evaluate(annotated, annotated, rows=400, sample_rate_hz=RATE)
    assert perfect.f1 == 1.0 and perfect.boundary_mae_s == 0.0
    assert mean_metrics([metrics, perfect])["matched"] == 5

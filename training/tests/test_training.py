# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""End to end on synthetic data: a small model learns to find the rallies and is exported."""

import json

import numpy as np
import onnxruntime
import pytest
import torch

from tests.synthetic import DIMS, write_video
from ttrally_training.data import select_videos
from ttrally_training.decoding import DEFAULT_PARAMS
from ttrally_training.export_detector import TOLERANCE, detector_metadata, export
from ttrally_training.model import ModelConfig
from ttrally_training.training import TrainConfig, cross_validate, evaluate_held_out, train
from ttrally_training.windows import WindowConfig

SMALL = TrainConfig(epochs=12, batch_size=4,
                    model=ModelConfig(hidden=16, levels=4, stages=2, input_dropout=0.0),
                    windows=WindowConfig(length_s=60.0))
CPU = torch.device("cpu")


@pytest.fixture(scope="module")
def videos(tmp_path_factory):
    root = tmp_path_factory.mktemp("data")
    for number in range(3):
        write_video(root, f"match_{number}", 2400, seed=number)
    return select_videos(root / "features", root / "annotations").videos


def test_held_out_videos_are_segmented(videos):
    groups = {v.video_id: v.video_id for v in videos}
    probabilities = cross_validate(videos, groups, SMALL, seed=1, runs=1, device=CPU,
                                   log=lambda _: None)
    evaluation = evaluate_held_out(videos, groups, probabilities)
    assert set(evaluation.per_video) == {v.video_id for v in videos}
    for metrics in evaluation.per_video.values():
        assert metrics.f1 > 0.8
        assert metrics.boundary_mae_s < 1.0
    assert evaluation.most_common_params()["method"] in {"threshold", "viterbi"}


def test_training_is_reproducible(videos):
    first = train(videos[:1], SMALL, seed=3, device=CPU, log=lambda _: None)
    second = train(videos[:1], SMALL, seed=3, device=CPU, log=lambda _: None)
    features = videos[1].features
    assert np.allclose(first.predict(features, CPU), second.predict(features, CPU))


def test_export_matches_pytorch_and_carries_the_metadata(videos, tmp_path):
    trained = train(videos, SMALL, seed=1, device=CPU, log=lambda _: None)
    out = tmp_path / "rally-detector.onnx"
    metadata = detector_metadata(videos[0].manifest, DEFAULT_PARAMS, {"videos": 3})
    assert export(trained, DIMS, metadata, out) < TOLERANCE

    session = onnxruntime.InferenceSession(str(out), providers=["CPUExecutionProvider"])
    stored = session.get_modelmeta().custom_metadata_map
    assert stored["ttrally.kind"] == "rally-detector"
    assert stored["ttrally.backbone"] == "fake-backbone (fp16)"
    assert json.loads(stored["ttrally.decoding"]) == DEFAULT_PARAMS
    features = videos[0].features[None]  # exactly as ttrally features writes them
    probability = session.run(None, {"features": features})[0][0]
    assert np.allclose(probability, trained.predict(videos[0].features, CPU), atol=1e-4)

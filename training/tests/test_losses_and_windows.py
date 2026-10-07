# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import numpy as np
import torch

from ttrally_training.losses import focal_loss, smooth_boundaries
from ttrally_training.windows import WindowConfig, WindowDataset


def test_labels_are_softened_only_near_boundaries():
    labels = np.r_[np.zeros(50), np.ones(50), np.zeros(50)].astype(np.float32)
    smoothed = smooth_boundaries(labels, sample_rate_hz=10.0, sigma_s=0.2, amount=0.2)
    assert smoothed[0] == 0.0 and smoothed[75] == 1.0  # far from boundaries
    assert 0.0 < smoothed[49] < 0.5 < smoothed[50] < 1.0
    assert np.isclose(smoothed[49], 1 - smoothed[50])  # symmetric around the boundary


def test_masked_rows_do_not_count():
    logits = torch.tensor([[3.0, -3.0, 50.0]])
    targets = torch.tensor([[1.0, 0.0, 0.0]])
    mask = torch.tensor([[1.0, 1.0, 0.0]])
    weight = torch.tensor([1.0])
    assert focal_loss(logits, targets, mask, weight) < 1e-3


def test_windows_cover_every_row_and_pad_short_videos():
    long_video = (np.ones((250, 4), np.float32), np.ones(250, np.float32))
    short_video = (np.ones((30, 4), np.float32), np.ones(30, np.float32))
    mean, std = np.full(4, 7.0, np.float32), np.ones(4, np.float32)
    dataset = WindowDataset([long_video, short_video], sample_rate_hz=10.0,
                            config=WindowConfig(length_s=10.0, overlap=0.5), feature_mean=mean,
                            feature_std=std, augment=False, rng=np.random.default_rng(0))
    starts = [start for index, start in dataset.windows if index == 0]
    assert starts == [0, 50, 100, 150]  # last window ends at the last row
    x, y, mask = dataset[len(dataset) - 1]  # the short video
    assert x.shape == (100, 4) and mask.sum() == 30
    assert torch.all(x[30:] == 7.0)  # padding with the mean

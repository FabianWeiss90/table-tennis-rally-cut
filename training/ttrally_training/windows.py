# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original file: src/supervised/common.py (SignalDataset)
# Changes: window length and temporal shift in seconds instead of frames at 120 fps; random
#   window offsets instead of rolling the signal around; padding mask; noise relative to the
#   feature spread instead of clipping signals to positive values.
"""Training windows: long videos are cut into overlapping windows of a few minutes."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import torch
from torch.utils.data import Dataset


@dataclass(frozen=True)
class WindowConfig:
    # About twice the receptive field of a stage (51 s): enough context per row, and short
    # windows with a large overlap give many optimisation steps per epoch on a few videos.
    length_s: float = 100.0
    overlap: float = 0.75
    max_shift_s: float = 1.0  # random offset of each window per epoch
    noise: float = 0.05  # Gaussian noise in units of the per-dimension standard deviation


class WindowDataset(Dataset):
    """Windows of (features, labels, mask) over several videos.

    Each video is (features (T, D), labels (T,), mask (T,)), the mask being 0 for rows that do not
    count (ignored sections). Windows shorter than the window length (short videos) are padded
    with the mean features, which the model normalises to zero, and masked.
    """

    def __init__(self, videos: list[tuple[np.ndarray, np.ndarray, np.ndarray]],
                 sample_rate_hz: float,
                 config: WindowConfig, feature_mean: np.ndarray, feature_std: np.ndarray,
                 augment: bool, rng: np.random.Generator):
        self.videos = videos
        self.length = max(1, round(config.length_s * sample_rate_hz))
        self.max_shift = round(config.max_shift_s * sample_rate_hz) if augment else 0
        self.noise = config.noise if augment else 0.0
        self.feature_mean = feature_mean.astype(np.float32)
        self.feature_std = feature_std.astype(np.float32)
        self.rng = rng
        stride = max(1, round(self.length * (1 - config.overlap)))
        self.windows: list[tuple[int, int]] = []  # (video index, start row)
        for index, (features, _, _) in enumerate(videos):
            last_start = max(0, len(features) - self.length)
            starts = list(range(0, last_start + 1, stride))
            if starts[-1] != last_start:
                starts.append(last_start)
            self.windows.extend((index, start) for start in starts)

    def __len__(self) -> int:
        return len(self.windows)

    def __getitem__(self, item: int) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        index, start = self.windows[item]
        features, labels, row_mask = self.videos[index]
        if self.max_shift:
            start = int(np.clip(start + self.rng.integers(-self.max_shift, self.max_shift + 1),
                                0, max(0, len(features) - self.length)))
        end = min(start + self.length, len(features))
        x = np.tile(self.feature_mean, (self.length, 1))
        y = np.zeros(self.length, dtype=np.float32)
        mask = np.zeros(self.length, dtype=np.float32)
        x[: end - start] = features[start:end]
        y[: end - start] = labels[start:end]
        mask[: end - start] = row_mask[start:end]
        if self.noise:
            x[: end - start] += (self.rng.standard_normal((end - start, x.shape[1]))
                                 .astype(np.float32) * self.noise * self.feature_std)
        return torch.from_numpy(x), torch.from_numpy(y), torch.from_numpy(mask)

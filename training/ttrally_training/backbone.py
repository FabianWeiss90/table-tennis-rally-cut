# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Image backbone used for the per-frame features: DINOv2 with spatial pooling.

The exported ONNX model takes normalised RGB images (N, 3, H, W) and returns per image the
concatenation of six vectors of the backbone's width:

    cls, mean, top_left, top_right, bottom_left, bottom_right

cls is the class token, mean the average of all patch tokens, and the last four are the averages
of the patch tokens in the four quadrants of the image (e.g. near and far player).
"""

from __future__ import annotations

import torch
from torch import nn

FEATURE_PARTS = ("cls", "mean", "top_left", "top_right", "bottom_left", "bottom_right")
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)
PATCH_SIZE = 14


class PooledBackbone(nn.Module):
    """DINOv2 followed by class-token, global and 2x2 quadrant pooling, for one input size.

    DINOv2 interpolates its position embeddings to the input size in every forward pass. The
    size is fixed here, so they are interpolated once and stored; the exported model then has no
    interpolation node, which some execution providers and float16 conversion do not support.
    """

    def __init__(self, dinov2: nn.Module, height: int, width: int) -> None:
        super().__init__()
        if height % PATCH_SIZE or width % PATCH_SIZE:
            raise ValueError(f"input size must be a multiple of {PATCH_SIZE}")
        self.dinov2 = dinov2
        self.rows = height // PATCH_SIZE
        self.cols = width // PATCH_SIZE
        embeddings = dinov2.embeddings
        with torch.no_grad():
            tokens = torch.zeros(1, 1 + self.rows * self.cols, dinov2.config.hidden_size)
            positions = embeddings.interpolate_pos_encoding(tokens, height, width)
        self.register_buffer("positions", positions, persistent=False)
        embeddings.interpolate_pos_encoding = self._fixed_positions

    def _fixed_positions(self, _embeddings: torch.Tensor, _height: int, _width: int) -> torch.Tensor:
        return self.positions

    def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
        tokens = self.dinov2(pixel_values=pixel_values).last_hidden_state
        cls = tokens[:, 0]
        patches = tokens[:, -self.rows * self.cols :]  # skips register tokens if the model has any
        grid = patches.reshape(patches.shape[0], self.rows, self.cols, patches.shape[-1])
        top, bottom = grid[:, : self.rows // 2], grid[:, self.rows // 2 :]
        half = self.cols // 2
        quadrants = [
            top[:, :, :half],
            top[:, :, half:],
            bottom[:, :, :half],
            bottom[:, :, half:],
        ]
        parts = [cls, patches.mean(dim=1)] + [q.mean(dim=(1, 2)) for q in quadrants]
        return torch.cat(parts, dim=-1)

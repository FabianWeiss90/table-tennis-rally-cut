# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original file: src/supervised/mstcn_model.py (focal_loss, smooth_labels_boundary)
# Changes: mask for padded rows; boundary smoothing width in seconds instead of frames at
#   120 fps, computed without SciPy.
"""Training loss: focal loss on every stage, with labels softened near rally boundaries."""

from __future__ import annotations

import numpy as np
import torch
import torch.nn.functional as F

FOCAL_GAMMA = 2.0
REFINEMENT_STAGE_WEIGHT = 1.5  # later stages count more, as in spin-detector


def focal_loss(logits: torch.Tensor, targets: torch.Tensor, mask: torch.Tensor,
               pos_weight: torch.Tensor, gamma: float = FOCAL_GAMMA) -> torch.Tensor:
    """Binary focal loss averaged over the rows where mask is 1; concentrates on hard rows,
    which are mostly the ones near rally boundaries."""
    bce = F.binary_cross_entropy_with_logits(logits, targets, pos_weight=pos_weight,
                                             reduction="none")
    p = torch.sigmoid(logits)
    pt = torch.where(targets > 0.5, p, 1 - p)
    loss = ((1 - pt) ** gamma) * bce * mask
    return loss.sum() / mask.sum().clamp_min(1.0)


def stage_loss(all_logits: list[torch.Tensor], targets: torch.Tensor, mask: torch.Tensor,
               pos_weight: torch.Tensor) -> torch.Tensor:
    """Sum of the focal losses of all stages."""
    total = torch.zeros((), device=targets.device)
    for stage, logits in enumerate(all_logits):
        weight = 1.0 if stage == 0 else REFINEMENT_STAGE_WEIGHT
        total = total + weight * focal_loss(logits, targets, mask, pos_weight)
    return total


def smooth_boundaries(labels: np.ndarray, sample_rate_hz: float, sigma_s: float,
                      amount: float) -> np.ndarray:
    """Moves labels towards 0.5 near rally boundaries (Gaussian weight over the distance to the
    nearest boundary): the exact boundary row is uncertain by a fraction of a second."""
    if amount <= 0.0 or len(labels) == 0:
        return labels.astype(np.float32)
    changes = np.flatnonzero(np.diff(labels) != 0)  # boundary between row i and i + 1
    if len(changes) == 0:
        return labels.astype(np.float32)
    rows = np.arange(len(labels))
    boundary_positions = changes + 0.5
    nearest = np.abs(rows[:, None] - boundary_positions[None, :]).min(axis=1)
    sigma_rows = sigma_s * sample_rate_hz
    weight = np.exp(-(nearest**2) / (2 * sigma_rows**2))
    return (labels * (1 - amount * weight) + 0.5 * amount * weight).astype(np.float32)

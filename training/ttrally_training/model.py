# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original file: src/supervised/mstcn_model.py (DilatedLayer, PredictionStage, MSTCN)
# Changes: input of per-frame image features (rows, dims) with normalisation and input dropout
#   inside the model; temperature and sigmoid in the exported inference wrapper; variants
#   (attention, boundary head) removed.
"""MS-TCN (Multi-Stage Temporal Convolutional Network, Abu Farha & Gall, CVPR 2019) for
per-row rally / no-rally segmentation of image feature sequences.

Stage 1 turns the features into rally logits; every further stage refines the previous stage's
probabilities together with its hidden features.
"""

from __future__ import annotations

from dataclasses import dataclass

import torch
import torch.nn.functional as F
from torch import nn


@dataclass(frozen=True)
class ModelConfig:
    hidden: int = 128
    levels: int = 8  # dilations 1..128: receptive field 511 rows (51 s at 10 per second)
    stages: int = 4
    kernel: int = 3
    dropout: float = 0.1
    input_dropout: float = 0.2  # drops whole feature dimensions during training

    def receptive_field(self) -> int:
        """Rows that influence one output row of a single stage."""
        return 1 + (self.kernel - 1) * (2**self.levels - 1)


class DilatedLayer(nn.Module):
    """Dilated temporal convolution with a residual connection."""

    def __init__(self, channels: int, kernel: int, dilation: int, dropout: float):
        super().__init__()
        self.conv = nn.Conv1d(channels, channels, kernel, dilation=dilation,
                              padding=dilation * (kernel - 1) // 2)
        self.norm = nn.BatchNorm1d(channels)
        self.dropout = nn.Dropout(dropout)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.dropout(F.relu(self.norm(self.conv(x))))


class PredictionStage(nn.Module):
    """Input projection, dilated layers and a one-channel output head."""

    def __init__(self, in_channels: int, config: ModelConfig):
        super().__init__()
        self.input_projection = nn.Conv1d(in_channels, config.hidden, 1)
        self.layers = nn.ModuleList(
            DilatedLayer(config.hidden, config.kernel, 2**i, config.dropout)
            for i in range(config.levels))
        self.head = nn.Conv1d(config.hidden, 1, 1)

    def forward(self, x: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """Returns (logits (B, T), hidden features (B, hidden, T))."""
        hidden = self.input_projection(x)
        for layer in self.layers:
            hidden = layer(hidden)
        return self.head(hidden).squeeze(1), hidden


class RallyMSTCN(nn.Module):
    """Features (B, T, D) -> rally logits of every stage, each (B, T).

    The per-dimension mean and standard deviation of the training features are stored in the
    model, so the exported model takes the features exactly as `ttrally features` writes them.
    """

    def __init__(self, dims: int, config: ModelConfig):
        super().__init__()
        self.config = config
        self.register_buffer("feature_mean", torch.zeros(dims))
        self.register_buffer("feature_std", torch.ones(dims))
        self.input_dropout = nn.Dropout1d(config.input_dropout)
        self.first_stage = PredictionStage(dims, config)
        self.refinement_stages = nn.ModuleList(
            PredictionStage(config.hidden + 1, config) for _ in range(config.stages - 1))

    def set_normalisation(self, mean: torch.Tensor, std: torch.Tensor) -> None:
        self.feature_mean.copy_(mean)
        self.feature_std.copy_(std.clamp_min(1e-6))

    def forward(self, features: torch.Tensor) -> list[torch.Tensor]:
        x = ((features - self.feature_mean) / self.feature_std).transpose(1, 2)  # (B, D, T)
        # Dropout1d drops whole channels; applied to (B, D, T) that means feature dimensions
        x = self.input_dropout(x)
        logits, hidden = self.first_stage(x)
        all_logits = [logits]
        for stage in self.refinement_stages:
            logits, hidden = stage(torch.cat([hidden, torch.sigmoid(logits).unsqueeze(1)], dim=1))
            all_logits.append(logits)
        return all_logits


class RallyProbability(nn.Module):
    """Inference wrapper for the export: features (1, T, D) -> rally probability (1, T) of the
    last stage, calibrated with the fitted temperature."""

    def __init__(self, model: RallyMSTCN, temperature: float):
        super().__init__()
        self.model = model
        self.register_buffer("temperature", torch.tensor(float(temperature)))

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        return torch.sigmoid(self.model(features)[-1] / self.temperature)

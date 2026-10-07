# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
# Adapted from spin-detector by Yuwei Ba (MIT License)
# https://github.com/ibigbug/spin-detector
# Original files: src/supervised/mstcn_model.py (train_one_fold, _calibrate_temperature,
#   run_lovo), src/supervised/common.py (sweep_params)
# Changes: feature normalisation and padding masks; leave-one-group-out instead of one video;
#   decoding parameters (threshold or Viterbi, in seconds) tuned on the other groups only;
#   reproducible random number generators.
"""Training of the rally detector and its evaluation by leave-one-group-out cross-validation.

Each video (or group of videos, e.g. one channel) is held out once: a model is trained on the
others and predicts the held-out one. The decoding parameters for a held-out group are chosen on
the predictions of the other groups only, so the reported metrics are not tuned on the data they
are measured on.
"""

from __future__ import annotations

import random
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np
import torch
import torch.nn.functional as F
from torch import nn
from torch.utils.data import DataLoader

from ttrally_training.data import Video
from ttrally_training.decoding import candidate_params, decode, segments_of
from ttrally_training.losses import smooth_boundaries, stage_loss
from ttrally_training.metrics import SegmentMetrics, evaluate
from ttrally_training.model import ModelConfig, RallyMSTCN
from ttrally_training.windows import WindowConfig, WindowDataset

Log = Callable[[str], None]
TEMPERATURES = np.linspace(0.3, 3.0, 55)
GRADIENT_CLIP = 1.0


@dataclass(frozen=True)
class TrainConfig:
    epochs: int = 120
    learning_rate: float = 5e-4
    weight_decay: float = 1e-4
    batch_size: int = 8
    warmup_epochs: int = 5
    label_smoothing: float = 0.2  # amount towards 0.5 at rally boundaries (0 = off)
    smoothing_sigma_s: float = 0.125  # 15 frames at 120 fps in spin-detector
    temperature_scaling: bool = True
    model: ModelConfig = field(default_factory=ModelConfig)
    windows: WindowConfig = field(default_factory=WindowConfig)


@dataclass
class TrainedModel:
    model: RallyMSTCN
    temperature: float

    def predict(self, features: np.ndarray, device: torch.device) -> np.ndarray:
        """Rally probability of every row of one video."""
        logits = predict_logits(self.model, features, device)
        return 1.0 / (1.0 + np.exp(-logits / self.temperature))


def seed_everything(seed: int) -> np.random.Generator:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    return np.random.default_rng(seed)


def feature_statistics(videos: list[Video]) -> tuple[np.ndarray, np.ndarray]:
    """Per-dimension mean and standard deviation over all rows of the videos."""
    rows = sum(len(v.features) for v in videos)
    total = sum(v.features.sum(axis=0, dtype=np.float64) for v in videos)
    mean = total / rows
    squares = sum(((v.features - mean) ** 2).sum(axis=0, dtype=np.float64) for v in videos)
    return mean.astype(np.float32), np.sqrt(squares / rows).astype(np.float32)


def predict_logits(model: RallyMSTCN, features: np.ndarray, device: torch.device) -> np.ndarray:
    """Last-stage logits for a whole video in one pass."""
    model.eval()
    with torch.no_grad():
        x = torch.from_numpy(np.ascontiguousarray(features)).unsqueeze(0).to(device)
        return model(x)[-1].squeeze(0).cpu().numpy().astype(np.float64)


def fit_temperature(model: RallyMSTCN, videos: list[Video], device: torch.device) -> float:
    """Temperature T minimising the log loss of sigmoid(logit / T) on the training videos; a
    single scalar, so fitting it on training data does not overfit."""
    logits = np.concatenate([predict_logits(model, v.features, device) for v in videos])
    labels = np.concatenate([v.labels for v in videos])
    logits_t, labels_t = torch.from_numpy(logits), torch.from_numpy(labels.astype(np.float64))
    losses = [F.binary_cross_entropy_with_logits(logits_t / t, labels_t).item()
              for t in TEMPERATURES]
    return float(TEMPERATURES[int(np.argmin(losses))])


def train(videos: list[Video], config: TrainConfig, seed: int, device: torch.device,
          log: Log = print) -> TrainedModel:
    """Trains one model on the given videos."""
    rng = seed_everything(seed)
    rate = videos[0].sample_rate_hz
    mean, std = feature_statistics(videos)
    targets = [smooth_boundaries(v.labels, rate, config.smoothing_sigma_s,
                                 config.label_smoothing) for v in videos]
    dataset = WindowDataset([(v.features, t) for v, t in zip(videos, targets)], rate,
                            config.windows, mean, std, augment=True, rng=rng)
    generator = torch.Generator().manual_seed(seed)
    loader = DataLoader(dataset, batch_size=config.batch_size, shuffle=True, generator=generator)

    model = RallyMSTCN(videos[0].features.shape[1], config.model)
    model.set_normalisation(torch.from_numpy(mean), torch.from_numpy(std))
    model.to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=config.learning_rate,
                                  weight_decay=config.weight_decay)
    scheduler = learning_rate_schedule(optimizer, config)
    positives = sum(float(v.labels.sum()) for v in videos)
    negatives = sum(len(v.labels) for v in videos) - positives
    pos_weight = torch.tensor([negatives / max(positives, 1.0)], device=device)

    for epoch in range(config.epochs):
        model.train()
        epoch_loss = 0.0
        for x, y, mask in loader:
            x, y, mask = x.to(device), y.to(device), mask.to(device)
            loss = stage_loss(model(x), y, mask, pos_weight)
            optimizer.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), GRADIENT_CLIP)
            optimizer.step()
            epoch_loss += loss.item()
        scheduler.step()
        if (epoch + 1) % 20 == 0 or epoch + 1 == config.epochs:
            log(f"    epoch {epoch + 1:>3}/{config.epochs}  loss {epoch_loss / len(loader):.4f}")

    temperature = fit_temperature(model, videos, device) if config.temperature_scaling else 1.0
    return TrainedModel(model, temperature)


def learning_rate_schedule(optimizer: torch.optim.Optimizer,
                           config: TrainConfig) -> torch.optim.lr_scheduler.LRScheduler:
    """Linear warm-up, then cosine decay."""
    warmup = max(1, min(config.warmup_epochs, config.epochs // 10))
    return torch.optim.lr_scheduler.SequentialLR(
        optimizer,
        [torch.optim.lr_scheduler.LinearLR(optimizer, start_factor=0.1, total_iters=warmup),
         torch.optim.lr_scheduler.CosineAnnealingLR(optimizer,
                                                    T_max=max(1, config.epochs - warmup))],
        milestones=[warmup])


def cross_validate(videos: list[Video], groups: dict[str, str], config: TrainConfig,
                   seed: int, runs: int, device: torch.device,
                   log: Log = print) -> dict[str, np.ndarray]:
    """Held-out rally probabilities of every video (mean over `runs` seeds)."""
    names = sorted(set(groups.values()))
    if len(names) < 2:
        raise ValueError("cross-validation needs videos from at least two groups")
    probabilities: dict[str, list[np.ndarray]] = {v.video_id: [] for v in videos}
    for run in range(runs):
        for fold, held_out in enumerate(names):
            log(f"  run {run + 1}/{runs}, fold {fold + 1}/{len(names)}: holding out {held_out}")
            training = [v for v in videos if groups[v.video_id] != held_out]
            trained = train(training, config, seed + run, device, log)
            for video in videos:
                if groups[video.video_id] == held_out:
                    probabilities[video.video_id].append(trained.predict(video.features, device))
    return {video_id: np.mean(p, axis=0) for video_id, p in probabilities.items()}


def annotated_segments(video: Video):
    return segments_of(video.labels > 0.5)


def score_params(params: dict, videos: list[Video],
                 probabilities: dict[str, np.ndarray]) -> float:
    """Mean segment F1 of the decoding parameters over the videos."""
    return float(np.mean([
        evaluate(decode(probabilities[v.video_id], v.sample_rate_hz, params),
                 annotated_segments(v), len(v.labels), v.sample_rate_hz).f1
        for v in videos]))


def best_params(videos: list[Video], probabilities: dict[str, np.ndarray]) -> dict:
    """Decoding parameters with the highest mean segment F1 on the videos."""
    return max(candidate_params(), key=lambda p: score_params(p, videos, probabilities))


@dataclass
class Evaluation:
    per_video: dict[str, SegmentMetrics]
    params_per_group: dict[str, dict]

    def most_common_params(self) -> dict:
        counts = Counter(tuple(sorted(p.items())) for p in self.params_per_group.values())
        return dict(counts.most_common(1)[0][0])


def evaluate_held_out(videos: list[Video], groups: dict[str, str],
                      probabilities: dict[str, np.ndarray]) -> Evaluation:
    """Metrics of each video, decoded with parameters tuned on the other groups only."""
    per_video: dict[str, SegmentMetrics] = {}
    params_per_group: dict[str, dict] = {}
    for held_out in sorted(set(groups.values())):
        others = [v for v in videos if groups[v.video_id] != held_out]
        params = best_params(others, probabilities)
        params_per_group[held_out] = params
        for video in videos:
            if groups[video.video_id] == held_out:
                predicted = decode(probabilities[video.video_id], video.sample_rate_hz, params)
                per_video[video.video_id] = evaluate(predicted, annotated_segments(video),
                                                     len(video.labels), video.sample_rate_hz)
    return Evaluation(per_video, params_per_group)

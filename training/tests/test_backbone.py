# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""The export-friendly attention gives the same features as the Transformers implementations."""

import pytest
import torch
from transformers import AttentionInterface, Dinov2Config, Dinov2Model

from ttrally_training.backbone import EXPORT_ATTENTION, PooledBackbone, export_attention

HEIGHT, WIDTH = 56, 98  # 4 x 7 patches


def tiny_dinov2(attention: str) -> Dinov2Model:
    torch.manual_seed(0)  # same random weights for every attention implementation
    config = Dinov2Config(hidden_size=32, num_hidden_layers=2, num_attention_heads=4,
                          intermediate_size=64, image_size=56, patch_size=14,
                          attn_implementation=attention)
    return Dinov2Model(config).eval()


@pytest.mark.parametrize("reference", ["eager", "sdpa"])
def test_export_attention_matches_transformers(reference):
    AttentionInterface.register(EXPORT_ATTENTION, export_attention)
    images = torch.randn(2, 3, HEIGHT, WIDTH)
    with torch.no_grad():
        ours = PooledBackbone(tiny_dinov2(EXPORT_ATTENTION), HEIGHT, WIDTH)(images)
        expected = PooledBackbone(tiny_dinov2(reference), HEIGHT, WIDTH)(images)
    assert ours.shape == (2, 6 * 32)
    assert torch.allclose(ours, expected, atol=1e-5)

# Training

Python side of ttrally, managed with [uv](https://docs.astral.sh/uv/) (Python 3.11–3.13). Python
is only needed to export the image model and to train the rally detector, never to use `ttrally`.

## Setup

```sh
cd training
uv sync
```

This installs CPU builds of PyTorch, Hugging Face Transformers, ONNX and ONNX Runtime into
`training/.venv`.

## Export the image model for `ttrally features`

```sh
uv run python -m ttrally_training.export_backbone --out ../data/models/dinov2-vitb14.onnx
```

Downloads DINOv2 ViT-B/14 (`facebook/dinov2-base`, Apache-2.0) from Hugging Face, adds the pooling
of `ttrally_training/backbone.py` (class token, average over the image and over its four
quadrants; 6 x 768 values per image), exports it to ONNX for an input of 392x224 pixels and checks
the exported model numerically against PyTorch. Input size, normalisation and output layout are
stored as model metadata, where `ttrally features` reads them. The model weights are not part of
the repository.

Options: `--model` (another DINOv2 variant, e.g. `facebook/dinov2-small`), `--height` and
`--width` (multiples of 14).

## Test model

```sh
uv run python -m ttrally_training.make_test_model --out ../tests/fixtures/tiny_backbone.onnx
```

Writes the tiny model with the same interface that the C++ tests use instead of the large one.

## Training

Not written yet (phase 4).

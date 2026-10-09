# Training

Python side of ttrally, managed with [uv](https://docs.astral.sh/uv/) (Python 3.11–3.13). Python
is only needed to export the image model and to train the rally detector, never to use `ttrally`.

## Setup

```sh
cd training
uv sync
```

This installs CPU builds of PyTorch, Hugging Face Transformers, ONNX, ONNX Script (used by
PyTorch's ONNX exporter) and ONNX Runtime into `training/.venv`.

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
`--width` (multiples of 14), `--precision fp16` (16-bit weights and computations, about four
times faster on GPUs with WebGPU; features differ slightly, see the main README).

```sh
uv run python -m ttrally_training.export_backbone --precision fp16 \
    --out ../data/models/dinov2-vitb14-fp16.onnx
```

The position embeddings are interpolated to the input size once during the export, so the model
works for exactly the exported input size. The attention is exported as plain softmax attention
(`export_attention` in `backbone.py`, identical results): the default implementation of
Transformers exports with checks and masks on every attention matrix that make the model up to
five times slower with WebGPU.

## Test model

```sh
uv run python -m ttrally_training.make_test_model --out ../tests/fixtures/tiny_backbone.onnx
```

Writes the tiny model with the same interface that the C++ tests use instead of the large one.

## Training the rally detector

```sh
uv run python -m ttrally_training.train
```

Trains the rally detector (an MS-TCN, adapted from spin-detector) on the features of all
annotated videos and exports it to `../weights/rally-detector.onnx`, with a report next to it
(`rally-detector.report.json`).

**Which videos are used.** Every video with labels in `../data/annotations/<video_id>.csv`, features in
`../data/features/<video_id>/` and a **complete review** in `annotate` (every segment and gap
done, see `data/annotations/<video_id>.review.csv`). Videos with open items are skipped with a note,
because an unchecked gap may hide a missed rally that would be learned as "no rally". All rallies
count, including those flagged `aborted_toss` or `let`. Ignored sections (rows flagged `ignore`)
count neither in training nor in the evaluation; predictions lying mostly inside one are not
counted as false alarms. All features must come from the same
image model variant, execution provider and settings (e.g. all from the fp16 model on WebGPU);
mixed features are refused.

**What happens.**

1. Cross-validation: each video is held out once, a model is trained on the others and predicts
   it. The decoding parameters (threshold or Viterbi, see below) for a held-out video are tuned on
   the predictions of the other videos only. The output shows segment F1, precision, recall and
   the mean boundary error in seconds per video and on average; these numbers estimate how well
   the detector works on a new video.
2. The decoding parameters that work best on all held-out predictions are chosen.
3. The final model is trained on all videos, calibrated (temperature scaling) and exported.

Per row of the features (10 per second), the model returns a rally probability; `ttrally detect`
turns these into rallies with the stored decoding: either a threshold with merging of short
interruptions and a minimum length, or a two-state Viterbi decoder with mean rally and pause
durations. All durations are in seconds, independent of the frame rate.

**Options.**

| Option | Default | Meaning |
|---|---|---|
| `--groups FILE` | one group per video | CSV `video_id,group`: videos of one group (e.g. one YouTube channel or the same players) are held out together, so the evaluation is not flattered by near-identical videos |
| `--runs N` | 1 | cross-validation runs with different seeds, averaged (more stable numbers) |
| `--epochs`, `--hidden`, `--levels`, `--stages` | 120, 128, 8, 4 | training length and model size |
| `--skip-evaluation` | – | train only the final model, with default decoding (also used automatically with fewer than two groups) |
| `--videos ID ...` | all annotated | restrict the videos |
| `--device` | `auto` | `cuda` if PyTorch sees a GPU (CUDA or ROCm build), else `cpu` |
| `--out`, `--report` | `../weights/rally-detector.onnx` | output files |

**Duration.** On a CPU with 12 threads, one epoch takes about 7 s per 30 minutes of training
video. Six videos of 10 minutes: about 30 minutes per model, about 3 hours including the
cross-validation (one model per held-out video). PyTorch is installed as a CPU build; a GPU build
shortens this considerably.

**Exported model.** Input `features` (1, rows, 4608) float32 exactly as `ttrally features` writes
them (normalisation is part of the model), output `rally_probability` (1, rows). Metadata:
`ttrally.kind`, `ttrally.backbone` (e.g. `facebook/dinov2-base (fp16)`),
`ttrally.backbone_execution_provider`, `ttrally.parts`, `ttrally.part_dims`, `ttrally.input_size`,
`ttrally.sample_rate_hz`, `ttrally.decoding` (JSON) and `ttrally.training` (JSON: videos, rallies,
temperature, cross-validation results). `ttrally detect` uses them to refuse features of another
model variant.

## Fixtures for the C++ tests

```sh
uv run python -m ttrally_training.make_detection_fixtures --out ../tests/fixtures
```

Writes a tiny untrained rally detector with the interface of the exported one (plus an input and
the probabilities PyTorch computes for it), and probabilities with the rallies the decoding finds
and how they are scored, for several decoding parameters. The C++ tests of `ttrally detect`
check that they reproduce these results exactly; run the script again after changing the
decoding, the metrics or the export.

## Tests

```sh
uv run pytest
```

Synthetic data only (no videos needed): label derivation, video selection, decoding, metrics,
training windows, a short training with cross-validation and the ONNX export.

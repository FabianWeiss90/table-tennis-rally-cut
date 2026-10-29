# Training

Python side of ttrally, managed with [uv](https://docs.astral.sh/uv/) (Python 3.11–3.13). Python
is only needed to export the image model and to train the rally detector, never to use `ttrally`.

## Setup

```sh
cd training
uv sync
```

This installs PyTorch, Hugging Face Transformers, ONNX, ONNX Script (used by PyTorch's ONNX
exporter) and ONNX Runtime into `training/.venv`. On Linux x86_64 PyTorch is the ROCm build
(about 15 GB in all), which trains on AMD GPUs such as the RX 7800 XT and falls back to the CPU
without one; elsewhere it is the CPU build. ROCm on Fedora is best-effort (not an officially
supported distribution); the build brings its own ROCm libraries, the GPU only has to be
accessible (user in the groups `video` and `render`).

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
uv run python -m ttrally_training.train --library /path/to/library
uv run python -m ttrally_training.train --library /path/to/library \
    --videos "2026-10-03-Club-Away/weiss-heinemeier" "2026-10-10-Home/picht-handte"
```

Trains the rally detector (an MS-TCN, adapted from spin-detector) on the chosen videos of a
[video library](../README.md#video-library), validates it on all its other annotated videos and
exports it to
`<library>/ttrally-data/models/rally-detector.onnx`, with a report next to it
(`rally-detector.report.json`). Videos are named `<match day>/<video id>`. The launcher
(`ttrally gui`, tab "Train") lists the usable ones by match day; the ticked ones are the training
videos.

Without `--library`, the videos come from `--features-dir` and `--annotations-dir` (default
`../data/features` and `../data/annotations`) and the model goes to
`../weights/rally-detector.onnx`.

**Which videos are used.** Every video with labels
(`ttrally-data/annotations/<video_id>.csv` in its match day), features
(`ttrally-data/features/<video_id>/`) and a **complete review** in `annotate` (every segment and
gap done, see `<video_id>.review.csv` next to the labels). The ones given with `--videos` are
the training videos, all others the validation videos. Videos with open items are not used,
because an unchecked gap may hide a missed rally that would be learned as "no rally". All rallies
count, including those flagged `aborted_toss` or `let`. Ignored sections (rows flagged `ignore`)
count neither in training nor in the evaluation; predictions lying mostly inside one are not
counted as false alarms. All features must come from the same
image model variant, execution provider and settings (e.g. all from the fp16 model on WebGPU);
mixed features are refused.

**What happens.**

1. The model is trained on the training videos and calibrated (temperature scaling).
2. Validation: it predicts the validation videos; the decoding parameters (threshold or Viterbi,
   see below) that work best on them are chosen. The output shows segment F1, precision, recall
   and the mean boundary error in seconds per validation video and on average. Since the decoding
   is chosen on the same videos, the numbers are slightly optimistic; they tell most when the
   validation videos come from other halls, camera positions or players than the training videos.
   Without validation videos, the default decoding is used.
3. The model is exported with the chosen decoding.

Per row of the features (10 per second), the model returns a rally probability; `ttrally detect`
turns these into rallies with the stored decoding: either a threshold with merging of short
interruptions and a minimum length, or a two-state Viterbi decoder with mean rally and pause
durations. All durations are in seconds, independent of the frame rate.

**Options.**

| Option | Default | Meaning |
|---|---|---|
| `--library DIR` | – | the video library (otherwise `--features-dir` and `--annotations-dir`) |
| `--epochs`, `--hidden`, `--levels`, `--stages` | 120, 128, 8, 4 | training length and model size |
| `--videos KEY ...` | all usable (then no validation) | the training videos (`<match day>/<video id>` in a library); all other usable videos validate |
| `--device` | `auto` | `cuda` if PyTorch sees a GPU (CUDA or ROCm build), else `cpu` |
| `--out`, `--report` | `<library>/ttrally-data/models/rally-detector.onnx` (without a library `../weights/…`) | output files |

**Duration.** One epoch takes about 2.5 s per 30 minutes of training video on an RX 7800 XT
(ROCm) and about 6.5 s on a CPU with 12 threads; loading the features and validating add about
half a minute. Eleven training videos of 15 minutes and 120 epochs: about half an hour on the
GPU, 1¼ hours on the CPU. On ROCm the training uses PyTorch's own convolutions instead of
MIOpen's, which are slower for this model.

**Exported model.** Input `features` (1, rows, 4608) float32 exactly as `ttrally features` writes
them (normalisation is part of the model), output `rally_probability` (1, rows). Metadata:
`ttrally.kind`, `ttrally.backbone` (e.g. `facebook/dinov2-base (fp16)`),
`ttrally.backbone_execution_provider`, `ttrally.parts`, `ttrally.part_dims`, `ttrally.input_size`,
`ttrally.sample_rate_hz`, `ttrally.decoding` (JSON) and `ttrally.training` (JSON: videos, rallies,
temperature, validation videos and results). `ttrally detect` uses them to refuse features of another
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
training windows, a short training with validation and the ONNX export.

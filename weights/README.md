<!-- SPDX-License-Identifier: CC-BY-NC-4.0 -->

# Model card

**No trained model has been published yet.** This card will be filled in once a model is trained.

## Intended use

Per-frame rally / no-rally segmentation of table tennis videos recorded from a fixed camera
**diagonally behind the table** (hobby/club perspective), as part of the `ttrally` pipeline.

## Model

- Architecture: MS-TCN on per-frame embeddings of a pretrained image backbone (to be decided)
- Input: embeddings at 10 fps
- Output: per-frame rally probability, decoded into segments by `ttrally detect`
- Format: ONNX (`*.onnx`); optional training checkpoints as `*.safetensors` (stored via Git LFS)

The backbone weights are **not** stored here; `training/` contains a script to export them
locally.

## Training data

- Perspective: diagonal view from behind the table
- Number of videos: –
- Number of rallies: –
- Videos are not published; only the annotations in `annotations/` are.

## Evaluation

Leave-one-video-out evaluation.

| Metric | Value |
|---|---|
| Segment F1 | – |
| Boundary MAE (s) | – |

## Known limitations

- Not suitable for side-view footage (use spin-detector or similar for that).
- To be completed.

## License

Copyright (c) 2026 Fabian Weiß. The model weights in this directory are licensed under the
[Creative Commons Attribution-NonCommercial 4.0 International License (CC BY-NC 4.0)](LICENSE).

# table-tennis-rally-cut

Automatic rally detection for untrimmed table tennis videos recorded from a **hobby/club camera
perspective**: a fixed camera on a tripod, looking **diagonally from behind the table**, typically
recording 4K at 60 fps. The goal is a tool (`ttrally`) that cuts long recordings down to the
rallies.

Existing work such as spin-detector and OpenTTGames targets a **side view**. Its hand-crafted
signals do not transfer to the view from behind the table, so this project uses a learned,
purely visual model instead. Audio is **not** used for detection, because club halls are loud and
neighbouring tables are audible. Audio is used only to align videos during dataset creation.

## Status

**Early and experimental.** The first tool, `align`, is implemented and tested on synthetic and
generated media, but not yet verified on real match footage. Nothing here is ready for end users
yet, and commands, file formats and results may change without notice.

| Phase | Component | Status |
|---|---|---|
| 0 | Repository skeleton, build system, licensing | done |
| 1 | `align`, `devices` | implemented, verification on real footage pending |
| later | `annotate`, `features`, training, `detect`, `refine`, `cut`, `benchmark` | planned |

## Pipeline overview

1. **External pre-cut:** the untrimmed video is uploaded to [Liimba](https://liimba.com), which
   cuts it to rallies (with padding, Full HD). This happens outside this project.
2. **`align`:** aligns the Liimba cut against the 4K original via audio cross-correlation. The
   result is a list of segments with times and frame indices in the original, which serve as
   annotation candidates.
3. **`annotate`:** a small GUI that jumps to each candidate in the original. The user sets the
   exact start and end frame and quickly reviews the gaps between segments for missed rallies.
4. **`features`:** the original is sampled at 10 fps, downscaled, and turned into per-frame
   embeddings with a pretrained image model via ONNX Runtime. The embeddings are cached.
5. **Training (Python, `training/`):** an MS-TCN is trained on the embeddings with per-frame
   labels "rally / no rally" and exported to ONNX.
6. **`detect`:** C++ inference of the ONNX model and decoding into rally segments.
7. **`refine`** (optional): frame-accurate boundaries using all native-rate frames around each
   predicted boundary.
8. **`cut`:** cuts the 4K video according to the resulting cut list.

Everything that touches video runs in C++. Python is only needed to **train** models, never to
**use** the tool. Because the C++ tool also produces the training features, preprocessing is
identical between training and inference.

## Build and installation

Building `ttrally` takes three steps on every platform:

1. install the system prerequisites for your operating system,
2. set up [vcpkg](https://github.com/microsoft/vcpkg), the C++ package manager that provides
   CLI11, pocketfft and Catch2 (and FFmpeg on Windows),
3. configure and build with a CMake preset.

vcpkg runs in manifest mode: the dependencies are declared in `vcpkg.json` and are downloaded and
built automatically the first time you configure the project. vcpkg is located through the
environment variable `VCPKG_ROOT`, which must point to the vcpkg directory.

### Step 1: System prerequisites

#### Fedora

Fedora's default `ffmpeg-free` packages and Mesa VA drivers do not include H.264/HEVC for patent
reasons. Camera footage and VAAPI hardware decoding therefore require the
[RPM Fusion](https://rpmfusion.org/) packages:

```sh
# Enable RPM Fusion (free)
sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm

# FFmpeg with H.264/HEVC, development headers, and VAAPI drivers with H.264/HEVC
sudo dnf swap ffmpeg-free ffmpeg --allowerasing
sudo dnf install ffmpeg-devel
sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld

# Compiler, build tools, and the tools vcpkg needs
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config git git-lfs curl zip unzip tar
```

#### Debian / Ubuntu

```sh
# Compiler, build tools, and the tools vcpkg needs
sudo apt install build-essential cmake ninja-build pkg-config git git-lfs curl zip unzip tar

# FFmpeg development headers and VAAPI drivers
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    mesa-va-drivers vainfo
```

A C++20 compiler and CMake ≥ 3.25 are required. Debian 12 (bookworm) and Ubuntu 24.04 or newer
ship suitable versions.

#### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake ninja git git-lfs curl zip unzip tar ffmpeg libva-utils
```

On Arch, the FFmpeg package includes the development headers, and the VAAPI driver for AMD and
Intel GPUs is part of Mesa.

#### Linux: all distributions

Enable Git LFS once for your user (needed to download trained model weights):

```sh
git lfs install
```

You can check VAAPI hardware decoding with `vainfo`; it should list H.264 and HEVC profiles.

#### Windows

1. Install **Visual Studio 2022 or newer** with the workload "Desktop development with C++". It
   includes the MSVC compiler, CMake and Ninja. The free Community edition is sufficient.
2. Install **Git for Windows**; it includes Git LFS.

Both can be installed with `winget`:

```powershell
winget install --id Microsoft.VisualStudio.2022.Community -e --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive"
winget install --id Git.Git -e
git lfs install
```

FFmpeg does not need to be installed on Windows; vcpkg builds it as shared libraries without
GPL/nonfree components.

### Step 2: Set up vcpkg

vcpkg is installed once per machine, outside this repository.

**Linux:**

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Make VCPKG_ROOT permanent (bash; adapt for other shells)
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.bashrc
echo 'export PATH="$VCPKG_ROOT:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

**Windows** (PowerShell):

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics

# Make VCPKG_ROOT permanent, then open a new terminal
setx VCPKG_ROOT C:\vcpkg
```

Visual Studio also ships its own copy of vcpkg. Using a separate clone as shown above keeps
command-line builds independent of the Visual Studio installation.

### Step 3: Build

**Linux:**

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

The executable is then at `build/linux-release/src/ttrally`.

Available presets: `linux-release`, `linux-debug`, `linux-debug-sanitize` (ASan/UBSan) and
`ci-linux` (release with warnings as errors). On Fedora, the sanitizer preset additionally needs
`sudo dnf install libasan libubsan`.

**Windows:** open a **Developer PowerShell for VS** (from the Start menu) in the repository
directory and run:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The executable is then at `build\windows-release\src\ttrally.exe`. The first configuration
takes a while because vcpkg builds FFmpeg.

Available presets: `windows-release`, `windows-debug` and `ci-windows`.

> **FFmpeg licensing:** `ttrally` links to FFmpeg dynamically and needs only its LGPL parts.
> Linux distribution FFmpeg packages are often built with `--enable-gpl`. That is fine for
> building and using `ttrally` locally. Anyone who **redistributes** `ttrally` binaries must use
> an LGPL-only FFmpeg build. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

### Training environment (optional)

Only needed for training models. Requires Python ≥ 3.11 and [uv](https://docs.astral.sh/uv/).

| OS | Install uv |
|---|---|
| Fedora | `sudo dnf install uv` |
| Arch Linux | `sudo pacman -S uv` |
| Debian / Ubuntu | `curl -LsSf https://astral.sh/uv/install.sh \| sh` |
| Windows | `winget install --id astral-sh.uv -e` |

Then:

```sh
cd training
uv sync
```

Training runs on the CPU. A GPU is optional: CUDA, or ROCm builds of PyTorch on Linux. ROCm on
Fedora is best-effort, because Fedora is not an officially supported ROCm distribution.
The training code is not written yet.

## GPU support

GPU acceleration is used automatically when available, with a CPU fallback. Every selection can
be overridden on the command line.

**Video decoding** (FFmpeg hwaccel), probed in this order; the first one that works is used:

| Platform | Order |
|---|---|
| Linux | VAAPI (AMD/Intel) → CUDA/NVDEC (NVIDIA) → Vulkan video → software |
| Windows | D3D12VA → D3D11VA → CUDA/NVDEC → software |

Override: `--decode-backend auto|vaapi|cuda|d3d11va|d3d12va|vulkan|cpu`

**Neural network inference** (ONNX Runtime execution providers, later phases):

1. CUDA / TensorRT (NVIDIA)
2. MIGraphX (AMD, Linux only, requires ROCm; optional)
3. WebGPU (default for everyone else: Vulkan on Linux, D3D12 on Windows; no ROCm needed)
4. CPU

Override: `--ep auto|cuda|tensorrt|migraphx|webgpu|cpu`

AMD Radeon cards such as the **RX 7800 XT** are supported through **VAAPI** for decoding and
**WebGPU** for inference, with no ROCm installation required.

**Drivers:**

| OS | AMD / Intel | NVIDIA |
|---|---|---|
| Linux | Mesa (installed by default; VAAPI drivers see [Step 1](#step-1-system-prerequisites)) | proprietary NVIDIA driver (for NVDEC and CUDA) |
| Windows | current GPU driver from AMD/Intel | current GPU driver from NVIDIA |

Without a supported GPU, decoding and inference run on the CPU.

`ttrally devices` lists the detected backends and shows which one `auto` would pick.

## Usage

### `align`

Aligns a Liimba cut video with the 4K original via audio cross-correlation:

```sh
ttrally align ORIGINAL CUT --out data/align/<video_id>.csv [--report data/align/<video_id>.html]
```

Outputs:

- `<video_id>.csv`: one row per segment of the cut video and its time span in the original,
  as times and as frame indices at the original's frame rate:
  ```
  segment_id,cut_start_s,cut_end_s,orig_start_s,orig_end_s,orig_start_frame,orig_end_frame,orig_fps,offset_s,confidence
  ```
- `<video_id>.gaps.csv`: the parts of the original that are not in the cut video (between
  consecutive segments, before the first and after the last one), for checking for missed
  rallies:
  ```
  gap_id,after_segment_id,orig_start_s,orig_end_s,orig_start_frame,orig_end_frame,duration_s
  ```
- Optional HTML report: a single self-contained file with a summary, warnings, file properties
  (including audio and video start times), an offset-over-time plot, the segments and the gaps.

Times are presentation times in seconds on each file's own timeline. Frame indices are 0-based,
refer to the decoded frames of the original, and `orig_end_frame` is inclusive. The original's
frame rate is read from the file; variable frame rate is detected and handled via the frame
timestamps.

Options:

| Option | Default | Meaning |
|---|---|---|
| `--report FILE` | – | Write the HTML report |
| `--cache-dir DIR` | `data/cache` | Cache for decoded audio and frame timestamps |
| `--no-cache` | – | Neither read nor write the cache |
| `--decode-backend NAME` | `auto` | Video decoding for the visual spot check (see [GPU support](#gpu-support)) |
| `--no-visual-check` | – | Skip comparing one frame from the middle of each segment in both videos |
| `--min-confidence X` | `2` | Peak ratio below which a 2 s audio window counts as uncertain |
| `--local-search S` | `120` | Seconds searched after the previous match before searching the whole original |
| `--threads N` | all cores | Worker threads |
| `-v`, `--verbose` | – | Show FFmpeg diagnostics |

The first run reads the whole original once (decoding the audio and collecting the frame
timestamps), which is limited by disk speed for large 4K files. Later runs use the cache.

`align` aborts with an error if a file cannot be opened or has no audio stream, or if the audio
cannot be matched reliably, for example because of a music overlay. Visual alignment is not
implemented. Segment boundaries can be affected by fades in the cut video; they are only
candidates and are set exactly during annotation.

### `devices`

```sh
ttrally devices
```

Lists the hardware decode backends of this platform, whether they are available, and which one
`auto` selects. Example on Linux with an AMD GPU:

```
Video decode backends (probe order on this platform):
  vaapi    available
  cuda     not available (Operation not permitted)
  vulkan   available
  cpu      always available
auto -> vaapi
```

## Annotation definitions and label format

Labels follow these binding definitions:

- **Rally start:** the first frame in which the ball visibly leaves the palm during the service
  toss. Under ITTF rules the ball is in play from the moment it is projected, and this moment can
  be annotated consistently.
- **Rally end:** the frame in which the point is decided (second bounce on the same side, ball
  touching the floor, ball in the net with no further play, etc.).
- **Bouncing the ball before serving** is not part of the rally.
- **Aborted toss** (ball tossed and caught again): counts as a very short rally with the flag
  `aborted_toss`.
- **Serve contact** (optional): the frame of racket contact on the serve.
- Padding for nicer cuts is added only when cutting, never in the labels.
- Annotation is always done at the original's **native frame rate**. Lower rates, such as 10 fps
  for the model, are derived from it via timestamps.

Labels are stored in `annotations/<video_id>.csv`:

```
video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes
match_a,1,10234,10811,59.94,10262,,
match_a,2,12950,13104,59.94,,aborted_toss,
```

- Frames are 0-based and `end_frame` is inclusive. They refer to the decoded frames of the
  original, based on presentation timestamps.
- `fps` is the exact stream rate (e.g. `60000/1001` → 59.94).

## Data

**Videos are not published**, and neither are audio tracks, extracted frames or caches. Only the
annotation CSVs, which contain no personal data, and trained model weights are part of this
repository.

If you record your own footage: filming people requires their **consent**. Ask everyone visible
in the recording before you film, and before you share any footage.

## License

This project is **source-available**. It is **not** open source in the sense of the
[Open Source Definition](https://opensource.org/osd): you may use, modify and share it freely
for noncommercial purposes, but **commercial use is not permitted**.

| Part | License |
|---|---|
| Source code | [PolyForm Noncommercial 1.0.0](LICENSE) |
| Trained model weights (`weights/`) | [CC BY-NC 4.0](weights/LICENSE) |
| Code adapted from spin-detector | MIT, in addition to the project license (see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)) |
| Third-party libraries | their respective licenses (see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)) |

Copyright (c) 2026 Fabian Weiß. Contributions are welcome under the terms in
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## Credits and citation

This project builds on the following work:

- **spin-detector** by Yuwei Ba: rally segmentation for side-view table tennis video. The MS-TCN
  training setup, Viterbi decoding and evaluation metrics are or will be adapted from it.
  <https://github.com/ibigbug/spin-detector>
- **MS-TCN** by Abu Farha and Gall: the temporal segmentation architecture.

```bibtex
@article{ba2026rally,
  title   = {Annotation-Free Rally Detection in Table Tennis Match Video:
             From Signal Fusion to Supervised Refinement},
  author  = {Ba, Yuwei},
  year    = {2026}
}

@inproceedings{farha2019mstcn,
  title     = {MS-TCN: Multi-Stage Temporal Convolutional Network for Action Segmentation},
  author    = {Abu Farha, Yazan and Gall, Juergen},
  booktitle = {Proceedings of the IEEE/CVF Conference on Computer Vision and Pattern Recognition (CVPR)},
  year      = {2019}
}
```

To cite this project, use the metadata in [`CITATION.cff`](CITATION.cff). GitHub shows it under
"Cite this repository".

## Liimba

This project uses videos cut by [Liimba](https://liimba.com) as an input for creating training
data. It is not affiliated with Liimba. Check Liimba's terms of service before reusing its output,
especially before using it for training data or sharing it.

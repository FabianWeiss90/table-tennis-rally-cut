# table-tennis-rally-cut

Automatic rally detection for untrimmed table tennis videos recorded from a **hobby/club camera
perspective**: a fixed camera on a tripod, looking **diagonally from behind the table**, typically
recording Full HD at 60 fps. The goal is a tool (`ttrally`) that cuts long recordings down to the
rallies.

Existing work such as spin-detector and OpenTTGames targets a **side view**. Its hand-crafted
signals do not transfer to the view from behind the table, so this project uses a learned,
purely visual model instead. Audio is **not** used for detection, because club halls are loud and
neighbouring tables are audible. Audio is used only to align videos during dataset creation.

## Status

**Early and experimental.** `align` and `annotate` are in use on real footage; `features`,
the training and `detect` are implemented, but no detector has been trained on real data yet.
Nothing here is ready for end users yet, and commands, file formats and results may change
without notice.

| Phase | Component | Status |
|---|---|---|
| 0 | Repository skeleton, build system, licensing | done |
| 1 | `align`, `devices` | done |
| 2 | `annotate` (GUI) | done |
| 3 | `features` (image features with DINOv2) | done |
| 4 | training (Python, `training/`) | implemented, not yet run on real data |
| 6 | `detect` | implemented, waiting for a trained detector |
| 8 | `cut` | done |
| later | `benchmark` | planned |

## Pipeline overview

1. **External pre-cut:** the untrimmed video is uploaded to [Liimba](https://liimba.com), which
   cuts it to rallies (with padding, Full HD). This happens outside this project.
2. **`align`:** aligns the Liimba cut against the original via audio cross-correlation. The
   result is a list of segments with times and frame indices in the original, which serve as
   annotation candidates.
3. **`annotate`:** a small GUI that jumps to each candidate in the original. The user sets the
   exact start and end frame and quickly reviews the gaps between segments for missed rallies.
4. **`features`:** the original is sampled at 10 fps, downscaled, and turned into per-frame
   embeddings with a pretrained image model via ONNX Runtime. The embeddings are cached.
5. **Training (Python, `training/`):** an MS-TCN is trained on the embeddings with per-frame
   labels "rally / no rally" and exported to ONNX.
6. **`detect`:** C++ inference of the ONNX model and decoding into rally segments.
7. **`cut`:** cuts the original video according to the resulting cut list, with a chosen margin
   before and after each rally.

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

# Compiler, build tools, and the tools vcpkg needs (autotools for the GUI dependencies)
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config git git-lfs curl zip unzip tar \
    autoconf autoconf-archive automake libtool
```

#### Debian / Ubuntu

```sh
# Compiler, build tools, and the tools vcpkg needs (autotools for the GUI dependencies)
sudo apt install build-essential cmake ninja-build pkg-config git git-lfs curl zip unzip tar \
    autoconf autoconf-archive automake libtool

# FFmpeg development headers and VAAPI drivers
sudo apt install libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev \
    libswresample-dev mesa-va-drivers vainfo
```

A C++20 compiler and CMake ≥ 3.25 are required. Debian 12 (bookworm) and Ubuntu 24.04 or newer
ship suitable versions.

#### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake ninja git git-lfs curl zip unzip tar ffmpeg libva-utils \
    autoconf-archive
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
`ci-linux` (release with warnings as errors).

The windows (`ttrally gui` and `ttrally annotate`, Dear ImGui + SDL3) are built by default. On Linux, vcpkg compiles SDL3 with
D-Bus and parts of systemd from source the first time, which takes a few minutes and needs the
autotools listed above. To build without the GUI (e.g. on a server), add
`-DTTRALLY_BUILD_GUI=OFF -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON -DVCPKG_MANIFEST_FEATURES=tests`
to the configure command. On Fedora, the sanitizer preset additionally needs
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

### Step 4: ONNX Runtime (for `features`)

`ttrally features` runs the image model with [ONNX Runtime](https://onnxruntime.ai/), which is
not installed by vcpkg but provided as a prebuilt directory. CMake looks for it in
`TTRALLY_ORT_ROOT` (`-DTTRALLY_ORT_ROOT=/path/to/onnxruntime`) or, if that is not set, in
`external/onnxruntime` and then `external/onnxruntime-cpu` inside the repository (`external/` is
not versioned). Without ONNX Runtime the build succeeds, but the `features` command is missing.

**CPU only** (simplest, works everywhere): the official release.

```sh
mkdir -p external/onnxruntime-cpu
curl -L https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-linux-x64-1.30.0.tgz \
    | tar -xz -C external/onnxruntime-cpu --strip-components=1
```

On Windows, use `onnxruntime-win-x64-1.30.0.zip` from the same release page.

**GPU via WebGPU** (AMD and Intel GPUs without ROCm, e.g. the RX 7800 XT): there is no official
Linux build with WebGPU, so ONNX Runtime is built from source. This takes a long time (about an
hour) and needs `patch` (`sudo dnf install patch` / `sudo apt install patch`) in addition to the
build tools above; the build downloads Dawn (Google's WebGPU implementation) by itself.

```sh
git clone --depth 1 --branch v1.30.0 --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/onnxruntime.git external/onnxruntime-src
cd external/onnxruntime-src
python3 tools/ci_build/build.py --build_dir ../onnxruntime-build --config Release \
    --build_shared_lib --parallel 12 --use_webgpu --skip_tests --skip_submodule_sync \
    --cmake_generator Ninja --compile_no_warning_as_error \
    --cmake_extra_defines CMAKE_INSTALL_PREFIX=$PWD/../onnxruntime onnxruntime_BUILD_UNIT_TESTS=OFF
cmake --install ../onnxruntime-build/Release
```

Reduce `--parallel` if the machine runs out of memory. After a new ONNX Runtime has been put in
place, configure again (`cmake --preset ...`). `ttrally devices` shows which execution providers
the ONNX Runtime in use offers.

### Training environment (optional)

Needed to export the image model for `features` and for training. Requires Python 3.11–3.13 and
[uv](https://docs.astral.sh/uv/).

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
`training/README.md` describes the export of the image model and the training of the rally
detector (`uv run python -m ttrally_training.train`); only videos whose review in `annotate` is
complete are used.

## GPU support

GPU acceleration is used automatically when available, with a CPU fallback. Every selection can
be overridden on the command line.

**Video decoding** (FFmpeg hwaccel), probed in this order; the first one that works is used:

| Platform | Order |
|---|---|
| Linux | VAAPI (AMD/Intel) → CUDA/NVDEC (NVIDIA) → Vulkan video → software |
| Windows | D3D12VA → D3D11VA → CUDA/NVDEC → software |

Override: `--decode-backend auto|vaapi|cuda|d3d11va|d3d12va|vulkan|cpu`

With hardware decoding, frames that are needed much smaller than recorded are first halved on
the GPU (FFmpeg filters such as `scale_vaapi`) and only then copied to system memory; the rest of
the scaling happens on the CPU, because GPU scalers alias visibly at larger factors.

Exception: while the image model of `features` runs on a GPU, `auto` decodes on the CPU (with all
cores). Hardware decoding would compete with the model for the GPU and slow both down, while the
CPU would sit idle; on an RX 7800 XT this makes `features` about 1.6 times faster.

**Neural network inference** (ONNX Runtime execution providers):

1. CUDA / TensorRT (NVIDIA)
2. MIGraphX (AMD, Linux only, requires ROCm; optional)
3. WebGPU (default for everyone else: Vulkan on Linux, D3D12 on Windows; no ROCm needed)
4. CPU

Override: `--ep auto|cuda|tensorrt|migraphx|webgpu|cpu`

AMD Radeon cards such as the **RX 7800 XT** are supported through **VAAPI** for decoding and
**WebGPU** for inference, with no ROCm installation required. Only providers compiled into the
ONNX Runtime in use are available: the official CPU release offers just the CPU, WebGPU needs the
own build described in [Step 4](#step-4-onnx-runtime-for-features).

**Drivers:**

| OS | AMD / Intel | NVIDIA |
|---|---|---|
| Linux | Mesa (installed by default; VAAPI drivers see [Step 1](#step-1-system-prerequisites)) | proprietary NVIDIA driver (for NVDEC and CUDA) |
| Windows | current GPU driver from AMD/Intel | current GPU driver from NVIDIA |

Without a supported GPU, decoding and inference run on the CPU.

`ttrally devices` lists the detected backends and shows which one `auto` would pick.

## Usage

The examples write `ttrally` for the built program (`build/linux-release/src/ttrally` on Linux,
`build\windows-release\src\ttrally.exe` on Windows). Run it from the repository directory.
The command-line examples below use the default paths under `data/`; the `gui` works on a
video library instead and fills in the paths in it.

### Video library

The videos can lie anywhere, e.g. on an external disk, in one folder per match day:

```
<library>/
  2026-10-03-Club-Away/              one folder per match day
    Weiss-Heinemeier_FHD.MP4         the untrimmed originals
    Liimba-Videos/                   any subfolder with "liimba" in its name
      4808_Weiss-Heinemeier_FHD.MP4  pre-cut versions (a number prefix or a suffix such as
                                     _cut_33_rallies is recognised)
    ttrally-data/                    created by the tool: everything it computes for this day
      align/  annotations/  features/  detections/  cuts/  cache/  scenes.csv
  ttrally-data/                      shared by all match days
    models/                          image models and trained rally detectors
```

Other subfolders (e.g. photos) are ignored. A video is named by its match day and its id, e.g.
`2026-10-03-Club-Away/weiss-heinemeier`. Caches are keyed by file name, size and modification
time, so a library can be moved or copied (keeping the modification times) without recomputing.

### `gui`

The easiest way to work: a window to run all commands without typing them. Start it from the
repository directory:

```sh
build/linux-release/src/ttrally gui          # Linux
build\windows-release\src\ttrally.exe gui     # Windows
```

On the first start, choose the [video library](#video-library) with "Choose..." at the top left;
the window remembers it (in `~/.config/ttrally/`, on Windows in `%APPDATA%\ttrally\`).

- **Left:** the videos of the library by match day, with their progress: aligned, labels
  (complete or in progress), features, detected, cut. Below, a preview frame of the selected
  video; the slider picks another position (the frame is decoded in the background).
- **Right:** a tab per command (align, annotate, features, detect, cut, train), filled in for
  the selected video: its pre-cut version, its id (suggested from the file name, e.g.
  `Weiss-Heinemeier_FHD.MP4` → `weiss-heinemeier`), the paths in the match day's
  `ttrally-data/`, and suggestions such as the image models and detectors in the library's
  `ttrally-data/models` and the detectors in `weights/`. Rarely needed options are folded away
  under "Advanced" with the commands' defaults. Fields for files have a "..." button for the file
  dialog.
- **Run:** the command runs as its own process; its output appears below, and it can be
  cancelled. The exact command line is shown above the button and can be copied. The list of
  videos is updated when a command has finished.
- **Train** runs the Python training with `uv` in `training/` (the uv program can be set under
  "Advanced"; a copy in `external/uv/` is found automatically). All videos with complete labels
  and features are listed by match day with a thumbnail (a click shows the video with its preview
  on the left); the ticked ones are trained on, all others validate the model. Each video has a
  **scene** (A, B, ...: the same hall seen from the same camera position): "Detect scenes" under
  each match day (in the video list and in the Train tab) suggests them by comparing the videos' backgrounds (the median of 15 frames, which
  removes the players); the choice in the details of the selected video and next to each video in
  the Train tab corrects them. They are stored in
  `<match day>/ttrally-data/scenes.csv`. A scene with videos in both training and validation is
  pointed out, since such a validation flatters the model (see [the training](training/README.md#training-the-rally-detector)).

Options: `--library` (the library to open, remembered for the next start), `--project`
(project directory with `weights/` and `training/`, default the current one).

### `align`

Aligns a Liimba cut video with the original via audio cross-correlation:

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
timestamps), which is limited by disk speed for long recordings. Later runs use the cache.

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

### `annotate`

Opens a window to set the exact start and end frame of every rally, starting from the
candidates found by `align`:

```sh
ttrally annotate data/original.mp4 --segments data/align/<video_id>.csv
```

The window shows the original frame by frame (hardware decoding as in `align`), a timeline of the
current segment or gap, and the list of all segments and gaps with their status.

**Without a pre-cut video** (no Liimba cut, e.g. a video from elsewhere), leave out `--segments`
and give the video id; every rally is then marked by hand:

```sh
ttrally annotate data/original.mp4 --video-id <video_id>
```

The whole video is one gap. Go through it and mark each rally; when you reach the end, press
**R** to check the gap, and the video is complete for training. Opened again later, the window
continues after the last marked rally. The launcher (`ttrally gui`) does this by itself for videos
without a recognised pre-cut version.

Annotating a rally takes three keys, anywhere in the video (inside a segment, in a gap, or
elsewhere):

1. **A** on the first frame in which the ball leaves the palm,
2. **D** on the frame in which the point is decided,
3. **Enter**: the rally is saved to `data/annotations/<video_id>.csv` and the window jumps to the
   next segment that has no rally yet (without segments it stays where it is).

**Ignored sections** leave a stretch of the video out of training, e.g. a rally whose start is
missing because the recording began late: mark it with **A** and **D** as well, then press
**I** instead of Enter. Ignored sections are no rallies; they must not overlap rallies, and a
segment containing one counts as done. Rather mark them a little too wide than too narrow.

Marks are never lost: if start and end are set, they are also saved when you switch to another
segment or close the window (as a rally, or as an ignored section if you were editing one).
Marks that are not saved yet are shown in red on the video.

**A video is finished when every segment and gap is done**, shown as `Segments n/n   Gaps m/m`
and "Complete" at the top of the list (next to the number of saved rallies, how many of them
are lets, and the number of ignored sections). Only finished videos are used for training, because an
unchecked gap may hide a missed rally that would be learned as "no rally":

- Segments that contain a saved rally are done automatically. Gaps are not: a rally reaching
  into a gap, or found in it, leaves the rest of the gap unchecked. Only a gap that rallies and
  ignored sections cover completely is done by itself.
- **X** marks a segment that contains no rally (e.g. a false match of the pre-cut).
- **R** marks a gap that you watched (e.g. at 4x speed) once all its rallies are marked, and jumps to
  the next gap that is not done yet.

The progress is saved next to the labels in `data/annotations/<video_id>.review.csv` (every
segment and gap with its frame range and status); progress kept by earlier versions in `data/annotate/` is taken over on the next start. Closing the window ends the
session; starting it again continues at the first open segment or gap. When you close the window
while segments have no rally or gaps were not checked, a dialog lists them; you can close anyway
or go back to the first of them.

Keyboard shortcuts (always shown below the video, together with a legend of the timeline colours):

| Keys | Action |
|---|---|
| Left / Right | one frame back / forward (Shift: 10 frames, Ctrl: 1 second) |
| Space, `[` / `]` | play / pause, slower / faster (0.1x to 4x) |
| A / D / C | mark start / end / serve hit at the current frame |
| T / L | toggle "aborted toss" / "let" |
| Enter / I | save the marks as a rally (go to the next segment) / as an ignored section |
| Esc | discard the marks |
| X | the segment contains no rally |
| R | the gap was checked and contains no rally; go to the next unchecked gap |
| O | reopen the item |
| Del | delete the saved rally or ignored section at the current frame |
| N / P | next / previous open item |
| Home / End | start / end of the current item |
| Mouse wheel | zoom into the video (drag to pan, double-click to reset) |

Options: `--gaps` (default `<segments>.gaps.csv`), `--video-id` (default: name of the segments
CSV), `--annotations-dir` (labels and review progress, default `data/annotations`),
`--decode-backend`, `--display-height` (default 1080) and `--frame-memory` (MB for decoded frames,
default 1024; more memory allows longer steps back without decoding again).

Saved rallies must follow the annotation rules below; the window refuses, for example,
overlapping rallies or an end before the start.

### `features`

Computes the image features that the rally detector is trained on and later runs on: every 0.1 s
of the video (on a regular time grid, independent of the frame rate), the frame shown at that
time is scaled to 392x224 pixels and described by the image model DINOv2 ViT-B/14. Per frame,
6 x 768 values are stored: the overall description, the average over the whole image and the
averages over its four quadrants (e.g. near and far player).

```sh
ttrally features data/original.mp4 --video-id <video_id>
```

The model is exported once with the training environment (see `training/README.md`) to
`data/models/dinov2-vitb14.onnx`. Results go to `data/features/<video_id>/`:

| File | Content |
|---|---|
| `features.npy` | float32, one row per 0.1 s, 4608 values per row |
| `times.npy` | float64, time of each row on the video's timeline (seconds) |
| `frames.npy` | int64, frame of the original used for each row |
| `manifest.json` | video, model, execution provider and settings the features were computed with |

Use the same `<video_id>` as for the labels, so that training can match features and labels. A
second run with the same video, model and settings does nothing; `--force` recomputes.

**Faster on GPUs: float16 model.** A model exported with `--precision fp16` runs about four times
faster with WebGPU (RX 7800 XT: about 9 instead of 34 ms per image) but is slower on the CPU. Its
features differ from those of the float32 model, with WebGPU by about 5 % on average. The rally
detector must therefore be trained on features from the same model variant, ideally computed on
the same execution provider, as the features it is later used with. The default is float32.

```sh
ttrally features data/original.mp4 --video-id <video_id> --model data/models/dinov2-vitb14-fp16.onnx
```

Options: `--model`, `--out-dir` (default `data/features`), `--ep` (execution provider, see
[GPU support](#gpu-support)), `--decode-backend`, `--rate` (samples per second, default 10) and
`--batch` (images per model run, default 16).

### `detect`

Finds the rallies of a video with a detector trained by `training/` (see
[`training/README.md`](training/README.md)):

```sh
ttrally detect data/original.mp4 --video-id <video_id>
```

1. **Features:** stored features in `data/features/<video_id>/` are used if they fit the
   detector; otherwise they are computed with the image model given by `--backbone` (default:
   the float16 model). The detector stores which image model, feature parts, sampling rate and
   image size it was trained with; features that differ are refused with an error, features
   computed on another execution provider give a warning (float16 values differ slightly
   between providers).
2. **Detection:** the detector computes a rally probability for every tenth of a second (on the
   CPU; it takes seconds even for long videos), and the decoding chosen during training (a
   threshold or a Viterbi decoder, stored in the model) turns them into rallies.
3. **Output** in `data/detections/`:

   | File | Content |
   |---|---|
   | `<video_id>.csv` | the rallies in the [label format](#annotation-definitions-and-label-format); notes: mean rally probability |
   | `<video_id>.probabilities.npy` | float32, rally probability of every feature row |

   Because the detections use the label format, they can be checked and corrected in the
   annotation window: `ttrally annotate ... --annotations-dir data/detections`.
4. **Evaluation:** if the video has labels in `data/annotations/`, the detections are compared
   with them: segment F1, precision, recall and the mean boundary error in seconds (the same
   definitions as in training; ignored sections do not count). A note appears if the review of
   the labels is not complete.

Options: `--detector` (default `weights/rally-detector.onnx`), `--backbone` (default
`data/models/dinov2-vitb14-fp16.onnx`), `--video-id`, `--features-dir`, `--out-dir` (default
`data/detections`), `--labels-dir` (default `data/annotations`), `--no-evaluation`, `--ep`,
`--decode-backend`, `--batch`.

To measure how well a detector works, train it without one of the videos
(`uv run python -m ttrally_training.train --videos <the others>`) and run `detect` on the left-out
video: the evaluation shows how it does on footage it has never seen.

### `cut`

Cuts a video down to its rallies, with a margin before and after each, into one MP4 file:

```sh
ttrally cut data/original.mp4 --video-id <video_id>                  # annotated or detected
ttrally cut data/original.mp4 --rallies data/detections/<video_id>.csv  # a chosen cut list
ttrally cut data/original.mp4 --video-id <video_id> --before 3 --after 1.5
```

- **Cut list:** any file in the [label format](#annotation-definitions-and-label-format). By
  default the annotations `data/annotations/<video_id>.csv` if they are complete (every segment
  and gap done in `annotate`), since they are exact; otherwise the detections
  `data/detections/<video_id>.csv` from `detect`. The launcher chooses the same way (annotations
  still in progress only if there are no detections) and offers the others as suggestions.
  Ignored sections are not rallies and are not cut.
- **Margins:** `--before` and `--after` (seconds, default 1 each). Rallies whose margins overlap
  or touch become one continuous section, so nothing is shown twice.
- **Frame-accurate (default):** every section starts and ends exactly at a frame; the video is
  re-encoded as **H.265 (HEVC)** with the GPU (VAAPI on AMD/Intel under Linux, NVENC, Quick Sync,
  AMF or Media Foundation). Without an H.265 encoder, H.264 is used (GPU, else OpenH264, else
  FFmpeg's own MPEG-4 encoder; no GPL encoders such as x264/x265). Colours and value range of
  the source are kept, the audio is encoded as AAC. A 10-minute match takes about 40 s on an
  RX 7800 XT. Before encoding, the output lists the video encoders this FFmpeg supports (in the
  order they are tried), those that cannot be opened on this machine, and the one used with its
  bit rate.
- **Bit rate:** `--bitrate` in Mbit/s, from 1. The default scales with resolution and frame rate: for
  Full HD at 60 fps **5 Mbit/s with H.265** (no loss visible on fast rallies; a cut is about
  70 % the size of the camera's original per minute) and 10 Mbit/s with H.264. Measured on fast
  rallies with the VAAPI H.265 encoder (PSNR against the original):

  | `--bitrate` | 5 | 6 | 7 | 8 | 12 | 20 |
  |---|---|---|---|---|---|---|
  | PSNR | 41.4 dB | 41.9 dB | 42.8 dB | 43.3 dB | 44.4 dB | 46.5 dB |
  | MB per minute | 39 | 47 | 54 | 62 | 92 | 151 |

  Below 5 Mbit/s the size goes down in proportion (about 7.5 MB per minute per Mbit/s, plus
  1.5 MB per minute of audio); the quality below 5 has not been measured.
- **H.265 playback:** VLC, mpv, current browsers, smartphones, Windows (with the HEVC
  extension) and YouTube play it. The files use the tag `hev1`; Apple's own players prefer
  `hvc1`, which the VAAPI encoder cannot produce correctly. For maximum compatibility use
  `--codec h264` (with VAAPI limited to 15 Mbit/s, because the driver hangs above).
- **`--fast`:** copies the compressed video without re-encoding (under a second), but each
  section is widened to the keyframes around it. Cameras often write a keyframe only every few
  seconds, so the cut can be much longer than asked (a 3:40 cut became 5:50 with keyframes every
  4 s).

Options: `--rallies`, `--annotations-dir` and `--detections-dir` (where the default looks,
`data/annotations` and `data/detections`), `--out` (default `data/cuts/<video_id>.mp4`), `--before`, `--after`,
`--fast`, `--codec` (`h265` or `h264`), `--bitrate` (Mbit/s), `--encoder` (e.g. `hevc_vaapi`,
`libopenh264`; default: the first of the codec that works), `--cache-dir`.

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
- **Let** (serve touching the net, replayed): counts as its own rally with the flag `let`. It
  starts with the toss like every rally and ends when play is stopped. A rally cannot be both an
  aborted toss and a let.
- **Serve contact** (optional): the frame of racket contact on the serve.
- **Ignored section:** a stretch of the video left out of training and evaluation, e.g. a rally
  whose start was not recorded. Not a rally; stored as a row with the flag `ignore` and an empty
  `rally_id`.
- Padding for nicer cuts is added only when cutting, never in the labels.
- Annotation is always done at the original's **native frame rate**. Lower rates, such as 10 fps
  for the model, are derived from it via timestamps.

Labels are stored in `data/annotations/<video_id>.csv`. Like the videos they belong to, they
stay local and are not part of the repository:

```
video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes
match_a,,0,890,59.94,,ignore,recording started mid-rally
match_a,1,10234,10811,59.94,10262,,
match_a,2,12950,13104,59.94,,aborted_toss,
```

- Frames are 0-based and `end_frame` is inclusive. They refer to the decoded frames of the
  original, based on presentation timestamps.
- Rows are sorted by start frame. Ignored sections have an empty `rally_id`, so the rallies are
  numbered 1, 2, 3, ... without gaps.
- `fps` is the exact stream rate (e.g. `60000/1001` → 59.94).

## Data

**Videos are not published**, and neither are audio tracks, extracted frames, caches or the
annotations (in the library's `ttrally-data/annotations/` or `data/annotations/`), which belong to
the videos and name the players in their video ids. Only the code and trained model weights are part of this repository.

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

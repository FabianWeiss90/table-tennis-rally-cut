# Third-party notices

This project (`ttrally`) uses or incorporates the third-party components listed below.
Each component remains under its own license. The project's own code is licensed under
PolyForm Noncommercial 1.0.0 (see [`LICENSE`](LICENSE)); trained model weights are licensed
under CC BY-NC 4.0 (see [`weights/LICENSE`](weights/LICENSE)).

## Libraries used by the C++ tool

| Component | Purpose | License | How it is obtained |
|---|---|---|---|
| [FFmpeg](https://ffmpeg.org/) (libavformat, libavcodec, libavutil, libswscale, libswresample) | Video/audio decoding, scaling, resampling | LGPL-2.1-or-later | Linux: system packages; Windows: vcpkg |
| [pocketfft](https://github.com/mreineck/pocketfft) (C++ header-only) | FFT for audio alignment | BSD-3-Clause | vcpkg |
| [CLI11](https://github.com/CLIUtils/CLI11) | Command-line parsing | BSD-3-Clause | vcpkg |
| [Catch2](https://github.com/catchorg/Catch2) v3 | Unit tests (not part of the shipped tool) | BSL-1.0 | vcpkg |
| [Dear ImGui](https://github.com/ocornut/imgui) | Annotation GUI | MIT | vcpkg (feature `gui`) |
| [SDL3](https://www.libsdl.org/) | Window, input and rendering of the annotation GUI | Zlib | vcpkg (feature `gui`) |

On Linux, vcpkg builds SDL3 with its default dependencies, among them
[D-Bus](https://www.freedesktop.org/wiki/Software/dbus/) (AFL-2.1 or GPL-2.0-or-later; used under
the AFL-2.1) and parts of [systemd](https://systemd.io/) (libsystemd, LGPL-2.1-or-later). They are
linked by SDL3 and are not modified. The default vcpkg triplet on Linux links them statically;
anyone who **redistributes** `ttrally` binaries must meet the LGPL requirements of libsystemd
(e.g. by providing the object files for relinking) or build without the GUI.

### FFmpeg (LGPL)

FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later.
`ttrally` links to the FFmpeg libraries **dynamically** and does not modify them. Users can
replace the FFmpeg shared libraries with their own compatible build.

The project requires FFmpeg features that are available under the LGPL only. Binaries of
`ttrally` that are distributed must be built against an FFmpeg build configured **without**
`--enable-gpl` and `--enable-nonfree`. Note that many distribution packages (e.g. RPM Fusion
on Fedora, Debian/Ubuntu) are built with `--enable-gpl`; this is fine for local, private
builds but not for redistributing binaries.

### Planned components (later phases, not used yet)

| Component | Purpose | License |
|---|---|---|
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) incl. WebGPU execution provider | Neural network inference | MIT |

This file will be updated when these components are integrated.

## Code adapted from spin-detector (MIT License)

Parts of this project are or will be adapted from
[spin-detector](https://github.com/ibigbug/spin-detector) by Yuwei Ba, which is licensed under
the MIT License. Every adopted, adapted or ported file carries a header naming the original
file and the changes made, and its SPDX identifier is
`PolyForm-Noncommercial-1.0.0 AND MIT`.

### Adopted files

None yet. Planned (later phases):

| Original file in spin-detector | Target in this project | Changes |
|---|---|---|
| `src/supervised/mstcn_model.py` | `training/` | adapted to embedding input |
| `src/evaluation/metrics.py` | `training/` and C++ evaluation | fps-independent parameters |
| `src/supervised/common.py` (`viterbi_decode`), `Rally` segment logic | C++ (`detect`) | ported to C++, fps-independent parameters |

### MIT License text of spin-detector

```
MIT License

Copyright (c) 2026 Yuwei Ba

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

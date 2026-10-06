# Contributing

Thank you for your interest in this project. Bug reports, ideas and pull requests are welcome.

## License of contributions

By submitting a contribution (code, documentation, annotations, model weights or any other
material) to this repository, you agree that:

1. your contribution is licensed under the licenses of this project: the
   [PolyForm Noncommercial License 1.0.0](LICENSE) for code and documentation, and
   [CC BY-NC 4.0](weights/LICENSE) for model weights; **and**
2. the copyright holder of this project (Fabian Weiß) may also license your contribution under
   other terms, including commercial terms.

You confirm that you have the right to grant these permissions, i.e. that the contribution is
your own work or that you are otherwise allowed to submit it under these conditions.

Do not submit code under licenses that are incompatible with the above (for example GPL or AGPL),
and do not submit data whose license does not allow this use.

## Guidelines

- All code, comments, documentation and commit messages are written in **English**.
- C++ code is formatted with the repository's `.clang-format` (LLVM base, 100 columns) and should
  pass the checks in `.clang-tidy`.
- Every source file (C++, CMake, Python) starts with the comment line
  `SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`. Files containing code adapted from
  third-party projects carry the additional license and an attribution header (see
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)).
- New dependencies must come from vcpkg or the system, must be compatible with the project
  licenses, and must be added to `THIRD_PARTY_NOTICES.md`. Do not use CMake
  `FetchContent`/`ExternalProject`.
- **Never commit videos, audio, extracted frames, caches or build directories.** Annotation CSVs
  in `annotations/` follow the label format described in the [README](README.md).
- Make sure all tests pass (`ctest --preset linux-debug` or the Windows equivalent) before
  opening a pull request.

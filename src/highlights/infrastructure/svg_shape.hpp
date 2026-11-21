// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <utility>
#include <string>
#include <vector>

struct NSVGimage;

namespace ttrally::highlights {

/// The SVG files in a folder (not its subfolders), by file name, sorted; empty if the folder
/// does not exist.
[[nodiscard]] std::vector<std::string> svg_files_in(const std::filesystem::path& folder);

/// An SVG drawing used as the silhouette of a title backdrop (read with nanosvg).
class SvgShape {
  public:
    /// Throws std::runtime_error if the file cannot be read or draws nothing.
    explicit SvgShape(const std::filesystem::path& file);
    ~SvgShape();
    SvgShape(const SvgShape&) = delete;
    SvgShape& operator=(const SvgShape&) = delete;

    /// Coverage (0-1) of the drawing fitted into width x height pixels, row by row: scaled to
    /// the height, then only its middle stretched or squeezed to the width, so that its ends
    /// (a third of it each, at most as wide as it is high) keep their shape. Narrower than both
    /// ends, the whole drawing is squeezed.
    [[nodiscard]] std::vector<float> coverage(int width, int height) const;

  private:
    NSVGimage* image_;
};

/// SVG shapes read once, again when their file changed.
class SvgShapeCache {
  public:
    /// Throws std::runtime_error if the file does not exist or cannot be read.
    const SvgShape& load(const std::filesystem::path& file);

  private:
    std::map<std::filesystem::path,
             std::pair<std::filesystem::file_time_type, std::unique_ptr<SvgShape>>>
        shapes_;
};

} // namespace ttrally::highlights

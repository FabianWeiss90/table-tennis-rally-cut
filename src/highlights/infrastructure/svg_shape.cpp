// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/svg_shape.hpp"

#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace ttrally::highlights {

namespace fs = std::filesystem;

namespace {

constexpr float kDpi = 96.0F;
constexpr double kEndShare = 1.0 / 3.0; ///< of the scaled drawing's width, per end
constexpr float kFullCoverage = 255.0F;

bool is_svg(const fs::path& file) {
    std::string extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".svg";
}

struct RasterizerDeleter {
    void operator()(NSVGrasterizer* rasterizer) const { nsvgDeleteRasterizer(rasterizer); }
};

} // namespace

std::vector<std::string> svg_files_in(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) {
        return names;
    }
    for (auto it = fs::directory_iterator(folder, error);
         !error && it != fs::directory_iterator(); it.increment(error)) {
        std::error_code type_error;
        if (it->is_regular_file(type_error) && is_svg(it->path())) {
            names.push_back(it->path().filename().string());
        }
    }
    std::ranges::sort(names);
    return names;
}

SvgShape::SvgShape(const fs::path& file) : image_(nullptr) {
    // Read here rather than by nanosvg, so that any path (e.g. with umlauts on Windows) works
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read the shape " + file.string());
    }
    std::string text(std::istreambuf_iterator<char>(in), {});
    image_ = nsvgParse(text.data(), "px", kDpi);
    if (image_ == nullptr || image_->shapes == nullptr || image_->width <= 0 ||
        image_->height <= 0) {
        if (image_ != nullptr) {
            nsvgDelete(image_);
        }
        throw std::runtime_error(file.string() + " is not an SVG drawing with a size and shapes");
    }
}

SvgShape::~SvgShape() { nsvgDelete(image_); }

std::vector<float> SvgShape::coverage(int width, int height) const {
    std::vector<float> result(static_cast<std::size_t>(std::max(0, width) * std::max(0, height)),
                              0.0F);
    if (width <= 0 || height <= 0) {
        return result;
    }
    const float scale = static_cast<float>(height) / image_->height;
    const int source = std::max(1, static_cast<int>(std::ceil(image_->width * scale)));
    std::vector<unsigned char> rgba(static_cast<std::size_t>(source) * height * 4, 0);
    const std::unique_ptr<NSVGrasterizer, RasterizerDeleter> rasterizer(nsvgCreateRasterizer());
    if (!rasterizer) {
        throw std::runtime_error("cannot draw SVG shapes (out of memory)");
    }
    nsvgRasterize(rasterizer.get(), image_, 0, 0, scale, rgba.data(), source, height, source * 4);
    // The ends keep their shape, at most as wide as the drawing is high
    const double end_px = std::min(source * kEndShare, static_cast<double>(height));
    for (int x = 0; x < width; ++x) {
        double column = 0;
        const double centre = x + 0.5;
        if (width < 2 * end_px) {
            column = centre * source / width;
        } else if (centre < end_px) {
            column = centre;
        } else if (centre >= width - end_px) {
            column = source - (width - centre);
        } else {
            column = end_px + (centre - end_px) * (source - 2 * end_px) / (width - 2 * end_px);
        }
        const double sample = std::clamp(column - 0.5, 0.0, source - 1.0);
        const int left = static_cast<int>(sample);
        const int right = std::min(source - 1, left + 1);
        const auto weight = static_cast<float>(sample - left);
        for (int y = 0; y < height; ++y) {
            const auto row = static_cast<std::size_t>(y) * source;
            const float a = rgba[4 * (row + left) + 3] / kFullCoverage;
            const float b = rgba[4 * (row + right) + 3] / kFullCoverage;
            result[static_cast<std::size_t>(y) * width + x] = a + (b - a) * weight;
        }
    }
    return result;
}

const SvgShape& SvgShapeCache::load(const fs::path& file) {
    std::error_code error;
    const auto changed = fs::last_write_time(file, error);
    if (error) {
        throw std::runtime_error("the shape " + file.filename().string() +
                                 " is not in the shapes folder " + file.parent_path().string());
    }
    auto& [time, shape] = shapes_[file];
    if (!shape || time != changed) {
        shape = std::make_unique<SvgShape>(file);
        time = changed;
    }
    return *shape;
}

} // namespace ttrally::highlights

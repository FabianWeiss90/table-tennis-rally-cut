// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/application/merge_videos.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>

namespace ttrally::cutting {

namespace {

constexpr std::size_t kMinParts = 2;

std::filesystem::path normal(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

void check_request(const MergeVideosRequest& request) {
    if (request.parts.size() < kMinParts) {
        throw std::invalid_argument("merging needs at least two parts");
    }
    std::vector<std::filesystem::path> seen;
    for (const auto& part : request.parts) {
        if (std::ranges::find(seen, normal(part)) != seen.end()) {
            throw std::invalid_argument(std::format("{} is given twice", part.string()));
        }
        seen.push_back(normal(part));
    }
    if (std::ranges::find(seen, normal(request.output)) != seen.end()) {
        throw std::invalid_argument(std::format(
            "the merged video {} would replace one of its parts; choose another file name",
            request.output.string()));
    }
}

} // namespace

MergeVideosResult MergeVideos::execute(const MergeVideosRequest& request) {
    check_request(request);
    const auto started = std::chrono::steady_clock::now();
    progress_.report(std::format("Merging {} parts into {}", request.parts.size(),
                                 request.output.string()));
    MergeVideosResult result;
    result.parts = request.parts.size();
    result.duration_s = joiner_.join(request.parts, request.output, progress_);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace ttrally::cutting

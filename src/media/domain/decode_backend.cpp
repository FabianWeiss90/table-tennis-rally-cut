// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/domain/decode_backend.hpp"

#include <array>

namespace ttrally::media {

namespace {

struct BackendName {
    DecodeBackend backend;
    std::string_view name;
};

constexpr std::array kBackendNames{
    BackendName{DecodeBackend::Auto, "auto"},       BackendName{DecodeBackend::Vaapi, "vaapi"},
    BackendName{DecodeBackend::Cuda, "cuda"},       BackendName{DecodeBackend::D3d11va, "d3d11va"},
    BackendName{DecodeBackend::D3d12va, "d3d12va"}, BackendName{DecodeBackend::Vulkan, "vulkan"},
    BackendName{DecodeBackend::Cpu, "cpu"},
};

} // namespace

std::string_view to_string(DecodeBackend backend) noexcept {
    for (const auto& entry : kBackendNames) {
        if (entry.backend == backend) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<DecodeBackend> parse_decode_backend(std::string_view name) noexcept {
    for (const auto& entry : kBackendNames) {
        if (entry.name == name) {
            return entry.backend;
        }
    }
    return std::nullopt;
}

std::vector<std::string> decode_backend_names() {
    std::vector<std::string> names;
    names.reserve(kBackendNames.size());
    for (const auto& entry : kBackendNames) {
        names.emplace_back(entry.name);
    }
    return names;
}

DecodeBackend select_automatic_backend(const std::vector<BackendStatus>& statuses) {
    for (const auto& status : statuses) {
        if (status.available) {
            return status.backend;
        }
    }
    return DecodeBackend::Cpu;
}

} // namespace ttrally::media

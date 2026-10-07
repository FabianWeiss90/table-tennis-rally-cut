// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/domain/execution_provider.hpp"

#include <array>
#include <utility>

namespace ttrally::features {

namespace {

constexpr std::array<std::pair<ExecutionProvider, std::string_view>, 6> kNames{{
    {ExecutionProvider::Auto, "auto"},
    {ExecutionProvider::Cuda, "cuda"},
    {ExecutionProvider::TensorRt, "tensorrt"},
    {ExecutionProvider::MiGraphX, "migraphx"},
    {ExecutionProvider::WebGpu, "webgpu"},
    {ExecutionProvider::Cpu, "cpu"},
}};

} // namespace

std::string_view to_string(ExecutionProvider provider) noexcept {
    for (const auto& [entry, name] : kNames) {
        if (entry == provider) {
            return name;
        }
    }
    return "unknown";
}

std::optional<ExecutionProvider> parse_execution_provider(std::string_view name) noexcept {
    for (const auto& [entry, text] : kNames) {
        if (text == name) {
            return entry;
        }
    }
    return std::nullopt;
}

std::vector<std::string> execution_provider_names() {
    std::vector<std::string> names;
    for (const auto& [entry, name] : kNames) {
        names.emplace_back(name);
    }
    return names;
}

std::vector<ExecutionProvider> automatic_provider_order() {
    return {ExecutionProvider::Cuda, ExecutionProvider::TensorRt, ExecutionProvider::MiGraphX,
            ExecutionProvider::WebGpu, ExecutionProvider::Cpu};
}

media::DecodeBackend feature_decode_backend(media::DecodeBackend requested,
                                            ExecutionProvider model) noexcept {
    if (requested != media::DecodeBackend::Auto) {
        return requested;
    }
    return model == ExecutionProvider::Cpu ? media::DecodeBackend::Auto : media::DecodeBackend::Cpu;
}

} // namespace ttrally::features

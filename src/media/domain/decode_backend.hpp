// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::media {

/// Video decode backend: a hardware acceleration API or software decoding.
enum class DecodeBackend { Auto, Vaapi, Cuda, D3d11va, D3d12va, Vulkan, Cpu };

[[nodiscard]] std::string_view to_string(DecodeBackend backend) noexcept;
[[nodiscard]] std::optional<DecodeBackend> parse_decode_backend(std::string_view name) noexcept;

/// All names accepted as a decode backend, including "auto" and "cpu".
[[nodiscard]] std::vector<std::string> decode_backend_names();

/// Availability of one hardware backend on this machine.
struct BackendStatus {
    DecodeBackend backend = DecodeBackend::Cpu;
    bool available = false;
    std::string detail; ///< Reason if not available
};

/// Policy for `auto`: the first available backend in probe order, otherwise software decoding.
[[nodiscard]] DecodeBackend select_automatic_backend(const std::vector<BackendStatus>& statuses);

} // namespace ttrally::media

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/devices_command.hpp"

#include "media/application/list_decode_backends.hpp"
#include "media/infrastructure/ffmpeg_decode_backend_probe.hpp"

#ifdef TTRALLY_WITH_ORT
#include "features/infrastructure/onnx_image_embedder.hpp"
#endif

#include <algorithm>
#include <format>
#include <iostream>

namespace ttrally::cli {

DevicesCommand::DevicesCommand(CLI::App& app)
    : command_(app.add_subcommand("devices", "List hardware decode backends")) {}

int DevicesCommand::run() {
    media::FfmpegDecodeBackendProbe probe;
    const auto overview = media::ListDecodeBackends(probe).execute();

    std::cout << "Video decode backends (probe order on this platform):\n";
    for (const auto& status : overview.hardware) {
        const std::string state =
            status.available ? "available" : "not available (" + status.detail + ")";
        std::cout << std::format("  {:<8} {}\n", media::to_string(status.backend), state);
    }
    std::cout << std::format("  {:<8} always available\n", "cpu");
    std::cout << std::format("auto -> {}\n", media::to_string(overview.automatic));
#ifdef TTRALLY_WITH_ORT
    const auto providers = features::compiled_execution_providers();
    std::cout << std::format("\nImage model execution providers (ONNX Runtime {}):\n",
                             features::onnxruntime_version());
    for (const auto provider : features::automatic_provider_order()) {
        const bool compiled =
            std::find(providers.begin(), providers.end(), provider) != providers.end();
        std::cout << std::format("  {:<9} {}\n", features::to_string(provider),
                                 compiled ? "compiled in" : "not in this build");
    }
    std::cout << "auto -> first of these that works on this machine\n";
#else
    std::cout << "\nImage model: ONNX Runtime not found at build time (TTRALLY_ORT_ROOT).\n";
#endif
    return 0;
}

} // namespace ttrally::cli

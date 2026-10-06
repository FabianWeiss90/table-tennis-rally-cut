// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/devices_command.hpp"

#include "media/application/list_decode_backends.hpp"
#include "media/infrastructure/ffmpeg_decode_backend_probe.hpp"

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
    std::cout << "\nInference execution providers: not integrated yet (later phase).\n";
    return 0;
}

} // namespace ttrally::cli

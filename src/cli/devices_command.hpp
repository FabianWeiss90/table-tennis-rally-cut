// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>

namespace ttrally::cli {

/// `ttrally devices`: lists hardware decode backends and the one `auto` selects.
class DevicesCommand {
  public:
    explicit DevicesCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
};

} // namespace ttrally::cli

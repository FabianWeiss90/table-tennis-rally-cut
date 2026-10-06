// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/application/progress_reporter.hpp"

#include <ostream>

namespace ttrally::cli {

/// Writes progress messages line by line to a stream (usually stdout).
class StreamProgressReporter final : public ProgressReporter {
  public:
    explicit StreamProgressReporter(std::ostream& out) : out_(out) {}
    void report(std::string_view message) override { out_ << message << '\n'; }

  private:
    std::ostream& out_;
};

} // namespace ttrally::cli

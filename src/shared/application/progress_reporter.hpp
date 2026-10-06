// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <string_view>

namespace ttrally {

/// Output port for human-readable progress messages of long-running use cases.
class ProgressReporter {
  public:
    virtual ~ProgressReporter() = default;
    virtual void report(std::string_view message) = 0;
};

/// Discards all messages.
class SilentProgressReporter final : public ProgressReporter {
  public:
    void report(std::string_view /*message*/) override {}
};

} // namespace ttrally

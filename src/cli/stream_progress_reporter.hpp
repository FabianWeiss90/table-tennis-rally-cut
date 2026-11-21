// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/application/progress_reporter.hpp"

#include <chrono>
#include <format>
#include <ostream>
#include <string>

namespace ttrally::cli {

/// Writes progress messages line by line to a stream (usually stdout).
class StreamProgressReporter final : public ProgressReporter {
  public:
    explicit StreamProgressReporter(std::ostream& out) : out_(out) {}
    void report(std::string_view message) override { out_ << message << '\n' << std::flush; }

  private:
    std::ostream& out_;
};

/// Puts the local time before every message, e.g. "[14:03:22] Part 1/4 ...".
class TimestampedProgressReporter final : public ProgressReporter {
  public:
    explicit TimestampedProgressReporter(ProgressReporter& inner) : inner_(inner) {}
    void report(std::string_view message) override {
        using std::chrono::floor;
        const auto now = floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const std::chrono::zoned_time local{std::chrono::current_zone(), now};
        inner_.report(std::format("[{:%H:%M:%S}] {}", local, message));
    }

  private:
    ProgressReporter& inner_;
};

} // namespace ttrally::cli

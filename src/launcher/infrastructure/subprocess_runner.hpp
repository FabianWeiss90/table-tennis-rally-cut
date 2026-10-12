// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace ttrally::launcher {

/// Runs programs as child processes (posix_spawn on Linux, CreateProcess on Windows) in their
/// own process group or job, so cancelling also ends the processes they start. A thread reads
/// the merged stdout/stderr.
class SubprocessRunner final : public ProcessRunner {
  public:
    SubprocessRunner();
    ~SubprocessRunner() override; ///< cancels a running program and waits for it
    SubprocessRunner(const SubprocessRunner&) = delete;
    SubprocessRunner& operator=(const SubprocessRunner&) = delete;

    void start(const Invocation& invocation) override;
    [[nodiscard]] bool running() const override;
    [[nodiscard]] std::string take_output() override;
    [[nodiscard]] std::optional<int> exit_code() const override;
    void cancel() override;

  private:
    struct Process; // platform specific
    void read_until_end();

    std::unique_ptr<Process> process_;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::string output_;
    std::optional<int> exit_code_;
    std::atomic<bool> running_ = false;
};

/// Path of the running executable (to start the tool's own commands).
[[nodiscard]] std::filesystem::path current_executable();

} // namespace ttrally::launcher

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace ttrally {

/// Number of worker threads to use when the user did not specify one.
[[nodiscard]] inline unsigned default_thread_count() noexcept {
    return std::max(1U, std::thread::hardware_concurrency());
}

/// Calls fn(i) for every i in [0, count) on up to `threads` threads.
/// The first exception thrown by fn is rethrown after all threads have finished.
template <typename Fn> void parallel_for(std::size_t count, unsigned threads, Fn&& fn) {
    if (count == 0) {
        return;
    }
    const auto workers = static_cast<unsigned>(std::min<std::size_t>(std::max(1U, threads), count));
    if (workers == 1) {
        for (std::size_t i = 0; i < count; ++i) {
            fn(i);
        }
        return;
    }

    std::atomic<std::size_t> next{0};
    std::exception_ptr error;
    std::mutex error_mutex;
    auto work = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count) {
                return;
            }
            try {
                fn(i);
            } catch (...) {
                const std::lock_guard lock(error_mutex);
                if (!error) {
                    error = std::current_exception();
                }
                next.store(count);
                return;
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (unsigned t = 1; t < workers; ++t) {
        pool.emplace_back(work);
    }
    work();
    for (auto& thread : pool) {
        thread.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

} // namespace ttrally

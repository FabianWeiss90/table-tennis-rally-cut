// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/white_balance.hpp"

#include <filesystem>

namespace ttrally::cutting {

/// Port: the white balance settings of a match day, kept in its work folder.
class WhiteBalanceStore {
  public:
    virtual ~WhiteBalanceStore() = default;
    /// Empty settings if none are stored.
    [[nodiscard]] virtual WhiteBalanceSettings load(const std::filesystem::path& work_dir) = 0;
    virtual void save(const std::filesystem::path& work_dir,
                      const WhiteBalanceSettings& settings) = 0;
};

} // namespace ttrally::cutting

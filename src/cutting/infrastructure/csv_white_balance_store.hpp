// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/white_balance_store.hpp"

#include <vector>

namespace ttrally::cutting {

/// File name of a match day's white balance per file, in its work folder.
inline constexpr std::string_view kWhiteBalanceFile = "white-balance.csv";

/// White balance in <work dir>/white-balance.csv (columns file,left,top,right,bottom: each
/// file's area, empty if taken from the file before) and the reference rally with its tuning in
/// <work dir>/white-balance-reference.csv (columns reference_video,reference_start,
/// reference_end,temperature,tint,brightness,match_brightness). Columns are found by their
/// names.
class CsvWhiteBalanceStore final : public WhiteBalanceStore {
  public:
    [[nodiscard]] WhiteBalanceSettings load(const std::filesystem::path& work_dir) override;
    void save(const std::filesystem::path& work_dir, const WhiteBalanceSettings& settings) override;
};

} // namespace ttrally::cutting

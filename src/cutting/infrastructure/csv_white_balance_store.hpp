// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/white_balance_store.hpp"

#include <vector>

namespace ttrally::cutting {

/// File name of a match day's white balance per file, in its work folder.
inline constexpr std::string_view kWhiteBalanceFile = "white-balance.csv";

/// White balance in <work dir>/white-balance.csv (columns file,choice,like,red,green,blue) and
/// the scenes' areas in <work dir>/white-balance-areas.csv (columns scene,left,top,right,bottom).
class CsvWhiteBalanceStore final : public WhiteBalanceStore {
  public:
    [[nodiscard]] WhiteBalanceSettings load(const std::filesystem::path& work_dir) override;
    void save(const std::filesystem::path& work_dir, const WhiteBalanceSettings& settings) override;
};

/// The gains for a video's timeline from a white balance file (white-balance.csv of its match
/// day): one span per part of a part list, one for a single file; files without gains are left
/// out. Empty if `white_balance` is empty or does not exist. Throws std::runtime_error if it
/// cannot be read.
[[nodiscard]] std::vector<ColorSpan> color_spans(const std::filesystem::path& video,
                                                 const std::filesystem::path& white_balance);

} // namespace ttrally::cutting

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"

namespace ttrally::launcher {

/// Matches of several files in <work dir>/matches.csv (columns match_id,part; the parts in
/// recording order), with each match's part list in <work dir>/matches/<id>.ffconcat.
class CsvMatchStore final : public MatchStore {
  public:
    [[nodiscard]] std::vector<Match> load(const std::filesystem::path& work_dir) override;
    void save(const std::filesystem::path& work_dir, const std::vector<Match>& matches) override;
};

} // namespace ttrally::launcher

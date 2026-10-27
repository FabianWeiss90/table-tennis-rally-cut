// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"

namespace ttrally::launcher {

/// Pre-cut videos in <work dir>/pre-cuts.csv (columns video_id,pre_cut). Paths inside the match
/// day folder (the work dir's parent) are stored relative to it, so the library can be moved.
class CsvPreCutStore final : public PreCutStore {
  public:
    [[nodiscard]] std::map<std::string, std::filesystem::path>
    load(const std::filesystem::path& work_dir) override;
    void save(const std::filesystem::path& work_dir,
              const std::map<std::string, std::filesystem::path>& pre_cuts) override;
};

} // namespace ttrally::launcher

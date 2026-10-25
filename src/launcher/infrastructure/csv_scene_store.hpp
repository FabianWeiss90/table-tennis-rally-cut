// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/detect_scenes.hpp"

namespace ttrally::launcher {

/// Scenes in <work dir>/scenes.csv (columns video_id,scene).
class CsvSceneStore final : public SceneStore {
  public:
    [[nodiscard]] std::map<std::string, std::string>
    load(const std::filesystem::path& work_dir) override;
    void save(const std::filesystem::path& work_dir,
              const std::map<std::string, std::string>& scenes) override;
};

} // namespace ttrally::launcher

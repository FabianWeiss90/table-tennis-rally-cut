// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/title_fonts.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ttrally::highlights {

class TitleFont;

/// The TrueType and OpenType fonts in the usual font folders of the system and of the user
/// (Linux: /usr/share/fonts, /usr/local/share/fonts, ~/.local/share/fonts, ~/.fonts; Windows:
/// the Windows fonts folder and the user's), written with stb_truetype.
class SystemTitleFonts final : public TitleFonts {
  public:
    SystemTitleFonts();
    ~SystemTitleFonts() override;
    SystemTitleFonts(const SystemTitleFonts&) = delete;
    SystemTitleFonts& operator=(const SystemTitleFonts&) = delete;

    /// For tests: only the fonts in these folders.
    explicit SystemTitleFonts(std::vector<std::filesystem::path> folders);

    [[nodiscard]] std::vector<FontChoice> installed() override;
    [[nodiscard]] FontChoice fallback() override;
    [[nodiscard]] TitleImage render(const std::filesystem::path& font, const Title& title,
                                    int line_height) override;

  private:
    const TitleFont& loaded(const std::filesystem::path& file);

    std::vector<std::filesystem::path> folders_;
    std::mutex mutex_; ///< installed() may run in the background
    std::map<std::filesystem::path, std::unique_ptr<TitleFont>> fonts_;
};

} // namespace ttrally::highlights

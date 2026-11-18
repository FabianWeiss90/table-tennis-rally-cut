// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/system_title_fonts.hpp"

#include "highlights/infrastructure/title_painter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>

namespace ttrally::highlights {

namespace fs = std::filesystem;

namespace {

/// Larger files are mostly fonts for Asian scripts, slow to read and not needed here.
constexpr std::uintmax_t kMaxFontBytes = 10U * 1024U * 1024U;

std::vector<fs::path> system_font_folders() {
    std::vector<fs::path> folders;
#ifdef _WIN32
    const char* windows = std::getenv("WINDIR");
    folders.push_back(fs::path(windows != nullptr ? windows : "C:\\Windows") / "Fonts");
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        folders.push_back(fs::path(local) / "Microsoft" / "Windows" / "Fonts");
    }
#else
    folders = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* home = std::getenv("HOME")) {
        folders.push_back(fs::path(home) / ".local" / "share" / "fonts");
        folders.push_back(fs::path(home) / ".fonts");
    }
#endif
    return folders;
}

bool is_font_file(const fs::path& file) {
    std::string extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".ttf" || extension == ".otf";
}

} // namespace

SystemTitleFonts::SystemTitleFonts() : folders_(system_font_folders()) {}

SystemTitleFonts::SystemTitleFonts(std::vector<fs::path> folders) : folders_(std::move(folders)) {}

SystemTitleFonts::~SystemTitleFonts() = default;

std::vector<FontChoice> SystemTitleFonts::installed() {
    std::vector<FontChoice> fonts;
    std::set<std::string> names;
    for (const fs::path& folder : folders_) {
        std::error_code error;
        if (!fs::is_directory(folder, error)) {
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(
                 folder, fs::directory_options::skip_permission_denied, error);
             !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            const fs::path& file = it->path();
            std::error_code size_error;
            if (!it->is_regular_file(size_error) || !is_font_file(file) ||
                fs::file_size(file, size_error) > kMaxFontBytes) {
                continue;
            }
            try {
                const TitleFont font(file);
                std::string name = font.name();
                if (!name.empty() && font.writes_latin() && names.insert(name).second) {
                    fonts.push_back({file, std::move(name)});
                }
            } catch (const std::exception&) {
                // not a font stb_truetype can read
            }
        }
    }
    std::ranges::sort(fonts, {}, &FontChoice::name);
    return fonts;
}

FontChoice SystemTitleFonts::fallback() {
    const fs::path file = default_title_font();
    if (file.empty()) {
        return {};
    }
    try {
        return {file, loaded(file).name()};
    } catch (const std::exception&) {
        return {file, file.stem().string()};
    }
}

TitleImage SystemTitleFonts::render(const fs::path& font, const Title& title, int line_height) {
    std::error_code error;
    // A chosen font that no longer exists is replaced by the default one, as in the video
    const fs::path file =
        font.empty() || !fs::is_regular_file(font, error) ? default_title_font() : font;
    if (file.empty()) {
        throw std::runtime_error("no font found for the titles");
    }
    return loaded(file).render(title.text, std::max(1, line_height), align_for(title.position));
}

const TitleFont& SystemTitleFonts::loaded(const fs::path& file) {
    const std::lock_guard lock(mutex_);
    auto& font = fonts_[file];
    if (!font) {
        font = std::make_unique<TitleFont>(file);
    }
    return *font;
}

} // namespace ttrally::highlights

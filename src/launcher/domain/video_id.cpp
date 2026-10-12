// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/video_id.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace ttrally::launcher {

namespace {

constexpr std::array<std::string_view, 9> kQualityTags{"fhd",   "uhd",   "hd",    "4k",  "2k",
                                                       "1080p", "720p",  "2160p", "60fps"};

std::vector<std::string> words_of(std::string_view stem) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : stem) {
        if (c == '_' || c == ' ' || c == '-' || c == '.') {
            if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
            continue;
        }
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    return words;
}

} // namespace

std::string suggest_video_id(std::string_view stem) {
    std::vector<std::string> words = words_of(stem);
    while (words.size() > 1 &&
           std::ranges::find(kQualityTags, std::string_view(words.back())) != kQualityTags.end()) {
        words.pop_back();
    }
    std::string id;
    for (const std::string& word : words) {
        id += (id.empty() ? "" : "-") + word;
    }
    return id.empty() ? "video" : id;
}

bool is_derived_from(std::string_view candidate, std::string_view original) {
    return candidate.size() > original.size() + 1 && candidate.starts_with(original) &&
           (candidate[original.size()] == '_' || candidate[original.size()] == '-' ||
            candidate[original.size()] == ' ');
}

} // namespace ttrally::launcher

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/matches.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace ttrally::launcher {

namespace {

constexpr std::size_t kMinCommonLength = 3; ///< shorter common starts name nothing

bool is_trimmed(char c) {
    return c == '-' || c == '_' || c == ' ' || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::string part_list_name(std::string_view id) { return std::string(id) + ".ffconcat"; }

std::string pre_cut_list_name(std::string_view id) {
    return std::string(id) + ".pre-cut.ffconcat";
}

std::string propose_match_id(std::span<const std::string> part_ids,
                             std::span<const std::string> taken) {
    if (part_ids.empty()) {
        return {};
    }
    std::string common = part_ids.front();
    for (const std::string& id : part_ids) {
        const auto [end, unused] = std::ranges::mismatch(common, id);
        common.erase(static_cast<std::size_t>(end - common.begin()));
    }
    while (!common.empty() && is_trimmed(common.back())) {
        common.pop_back();
    }
    if (common.size() < kMinCommonLength) {
        common = part_ids.front();
    }
    std::string proposed = common;
    for (int number = 2; std::ranges::find(taken, proposed) != taken.end(); ++number) {
        proposed = std::format("{}-{}", common, number);
    }
    return proposed;
}

bool is_valid_match_id(std::string_view id) {
    return !id.empty() && std::ranges::all_of(id, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
    });
}

} // namespace ttrally::launcher

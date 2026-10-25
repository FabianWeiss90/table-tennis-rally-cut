// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/scenes.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>

namespace ttrally::launcher {

namespace {

constexpr std::size_t kLetters = 26;

/// Mean and standard deviation of the pixels.
std::pair<double, double> statistics(const std::vector<float>& pixels) {
    const double mean =
        std::accumulate(pixels.begin(), pixels.end(), 0.0) / static_cast<double>(pixels.size());
    double squares = 0.0;
    for (const float pixel : pixels) {
        squares += (pixel - mean) * (pixel - mean);
    }
    return {mean, std::sqrt(squares / static_cast<double>(pixels.size()))};
}

double average_link(const std::vector<std::vector<double>>& similarities,
                    const std::vector<std::size_t>& a, const std::vector<std::size_t>& b) {
    double sum = 0.0;
    for (const std::size_t i : a) {
        for (const std::size_t j : b) {
            sum += similarities[i][j];
        }
    }
    return sum / static_cast<double>(a.size() * b.size());
}

} // namespace

double similarity(const Background& a, const Background& b) {
    if (a.width != b.width || a.height != b.height || a.pixels.empty() ||
        a.pixels.size() != b.pixels.size()) {
        return 0.0;
    }
    const auto [mean_a, deviation_a] = statistics(a.pixels);
    const auto [mean_b, deviation_b] = statistics(b.pixels);
    if (deviation_a == 0.0 || deviation_b == 0.0) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        sum += (a.pixels[i] - mean_a) * (b.pixels[i] - mean_b);
    }
    return sum / (static_cast<double>(a.pixels.size()) * deviation_a * deviation_b);
}

std::vector<std::string> group_into_scenes(const std::vector<std::vector<double>>& similarities,
                                           double threshold) {
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < similarities.size(); ++i) {
        groups.push_back({i});
    }
    for (;;) { // merge the most similar pair of groups while it is similar enough
        double best = threshold;
        std::optional<std::pair<std::size_t, std::size_t>> merge;
        for (std::size_t a = 0; a < groups.size(); ++a) {
            for (std::size_t b = a + 1; b < groups.size(); ++b) {
                const double link = average_link(similarities, groups[a], groups[b]);
                if (link >= best) {
                    best = link;
                    merge = {a, b};
                }
            }
        }
        if (!merge) {
            break;
        }
        auto& into = groups[merge->first];
        into.insert(into.end(), groups[merge->second].begin(), groups[merge->second].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(merge->second));
    }
    std::ranges::sort(groups, {}, [](const auto& group) { return std::ranges::min(group); });
    std::vector<std::string> names(similarities.size());
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (const std::size_t item : groups[g]) {
            names[item] = scene_name(g);
        }
    }
    return names;
}

std::string scene_name(std::size_t index) {
    std::string name;
    for (std::size_t rest = index + 1; rest > 0; rest = (rest - 1) / kLetters) {
        name.insert(name.begin(), static_cast<char>('A' + (rest - 1) % kLetters));
    }
    return name;
}

std::vector<std::string> scenes_in_both(const std::vector<VideoEntry>& videos,
                                        const std::vector<std::string>& trainable,
                                        const std::vector<std::string>& ticked) {
    struct Use {
        bool training = false;
        bool validation = false;
    };
    std::map<std::pair<std::string, std::string>, Use> uses;
    for (const VideoEntry& video : videos) {
        const std::string key = video.key();
        if (video.scene.empty() || std::ranges::find(trainable, key) == trainable.end()) {
            continue;
        }
        Use& use = uses[{video.match_day, video.scene}];
        (std::ranges::find(ticked, key) != ticked.end() ? use.training : use.validation) = true;
    }
    std::vector<std::string> both;
    for (const auto& [scene, use] : uses) {
        if (use.training && use.validation) {
            both.push_back(scene.first + ", scene " + scene.second);
        }
    }
    return both;
}

} // namespace ttrally::launcher

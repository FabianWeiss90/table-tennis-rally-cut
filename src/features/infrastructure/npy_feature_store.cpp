// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/infrastructure/npy_feature_store.hpp"

#include "shared/io/file_cache.hpp"
#include "shared/io/json.hpp"
#include "shared/io/npy.hpp"

#include <array>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ttrally::features {

namespace {

constexpr std::string_view kFingerprintKey = "\"fingerprint\": ";

std::string json_string(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            quoted += "\\\"";
            break;
        case '\\':
            quoted += "\\\\";
            break;
        case '\n':
            quoted += "\\n";
            break;
        default:
            quoted += c;
        }
    }
    return quoted + "\"";
}

std::string json_strings(const std::vector<std::string>& values) {
    std::string text = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        text += (i > 0 ? ", " : "") + json_string(values[i]);
    }
    return text + "]";
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

std::string manifest_fingerprint(const FeatureManifest& m) {
    std::string parts;
    for (const auto& part : m.parts) {
        parts += part + ",";
    }
    return std::format("{:016x}", io::fnv1a64(std::format(
                                      "{}|{}|{}|{}|{}|{}|{}|{}x{}|{}/{}|{}|{}", m.video_id,
                                      m.video_fingerprint, m.model_name, m.model_fingerprint,
                                      parts, m.part_dims, m.sample_rate_hz, m.input_width,
                                      m.input_height, m.video_fps.num, m.video_fps.den,
                                      m.video_frame_count, m.rows)));
}

std::string manifest_json(const FeatureManifest& m) {
    return std::format("{{\n"
                       "  \"format\": \"ttrally-features-1\",\n"
                       "  \"video_id\": {},\n"
                       "  \"video_path\": {},\n"
                       "  \"video_fps\": [{}, {}],\n"
                       "  \"video_frame_count\": {},\n"
                       "  \"sample_rate_hz\": {},\n"
                       "  \"rows\": {},\n"
                       "  \"dims\": {},\n"
                       "  \"parts\": {},\n"
                       "  \"part_dims\": {},\n"
                       "  \"model\": {},\n"
                       "  \"execution_provider\": {},\n"
                       "  \"input_size\": [{}, {}],\n"
                       "  {}{}\n"
                       "}}\n",
                       json_string(m.video_id), json_string(m.video_path), m.video_fps.num,
                       m.video_fps.den, m.video_frame_count, m.sample_rate_hz, m.rows,
                       m.parts.size() * m.part_dims, json_strings(m.parts), m.part_dims,
                       json_string(m.model_name), json_string(m.execution_provider), m.input_width, m.input_height, kFingerprintKey,
                       json_string(manifest_fingerprint(m)));
}

bool NpyFeatureStore::is_current(const FeatureManifest& expected) {
    const auto directory = directory_for(expected.video_id);
    for (const char* file : {"features.npy", "times.npy", "frames.npy", "manifest.json"}) {
        if (!std::filesystem::exists(directory / file)) {
            return false;
        }
    }
    const std::string stored = read_text(directory / "manifest.json");
    const std::string wanted =
        std::string(kFingerprintKey) + json_string(manifest_fingerprint(expected));
    return stored.find(wanted) != std::string::npos;
}

void NpyFeatureStore::save(const FeatureSet& features) {
    const FeatureManifest& m = features.manifest;
    const auto directory = directory_for(m.video_id);
    std::filesystem::create_directories(directory);
    // The manifest is removed first and written last, so an interrupted save is never current.
    std::filesystem::remove(directory / "manifest.json");

    const std::array<std::size_t, 2> matrix{m.rows, m.parts.size() * m.part_dims};
    const std::array<std::size_t, 1> vector{m.rows};
    io::write_npy<float>(directory / "features.npy", features.values, matrix);
    io::write_npy<double>(directory / "times.npy", features.times_s, vector);
    io::write_npy<std::int64_t>(directory / "frames.npy", features.frames, vector);
    io::write_file_atomically(directory / "manifest.json",
                              [&m](std::ofstream& out) { out << manifest_json(m); });
}

namespace {

FeatureManifest parse_manifest(const io::JsonValue& json) {
    FeatureManifest m;
    m.video_id = json.at("video_id").as_string();
    m.video_path = json.at("video_path").as_string();
    const auto& fps = json.at("video_fps").as_array();
    m.video_fps = {static_cast<std::int64_t>(fps.at(0).as_number()),
                   static_cast<std::int64_t>(fps.at(1).as_number())};
    m.video_frame_count = static_cast<std::int64_t>(json.at("video_frame_count").as_number());
    m.sample_rate_hz = json.at("sample_rate_hz").as_number();
    m.rows = static_cast<std::size_t>(json.at("rows").as_number());
    for (const auto& part : json.at("parts").as_array()) {
        m.parts.push_back(part.as_string());
    }
    m.part_dims = static_cast<std::size_t>(json.at("part_dims").as_number());
    m.model_name = json.at("model").as_string();
    if (json.contains("execution_provider")) { // missing in the first manifests
        m.execution_provider = json.at("execution_provider").as_string();
    }
    const auto& size = json.at("input_size").as_array();
    m.input_width = static_cast<int>(size.at(0).as_number());
    m.input_height = static_cast<int>(size.at(1).as_number());
    return m;
}

} // namespace

std::optional<FeatureSet> NpyFeatureStore::load(const std::string& video_id) {
    const auto directory = directory_for(video_id);
    if (!std::filesystem::exists(directory / "manifest.json")) {
        return std::nullopt;
    }
    FeatureSet features;
    try {
        features.manifest = parse_manifest(io::parse_json(read_text(directory / "manifest.json")));
    } catch (const io::JsonError& error) {
        throw std::runtime_error(std::format("{}: {}", (directory / "manifest.json").string(),
                                             error.what()));
    }
    auto values = io::read_npy<float>(directory / "features.npy");
    auto times = io::read_npy<double>(directory / "times.npy");
    auto frames = io::read_npy<std::int64_t>(directory / "frames.npy");
    const auto& m = features.manifest;
    const std::vector<std::size_t> matrix{m.rows, m.parts.size() * m.part_dims};
    if (values.shape != matrix || times.data.size() != m.rows || frames.data.size() != m.rows) {
        throw std::runtime_error(directory.string() + ": the stored files do not match the "
                                                      "manifest; compute the features again");
    }
    features.values = std::move(values.data);
    features.times_s = std::move(times.data);
    features.frames = std::move(frames.data);
    return features;
}

} // namespace ttrally::features

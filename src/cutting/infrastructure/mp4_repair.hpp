// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Checking and repairing the H.264/H.265 header of MP4 files written by earlier versions: the
// VAAPI encoder on Mesa wrote a header copy of the parameter sets that differs from the one in
// the stream, and H.265 was tagged hev1. Most players read the stream copy and do not notice;
// Apple's players read the header and need hvc1, so they played only the sound or nothing.

#pragma once

#include "cutting/infrastructure/parameter_sets.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ttrally::cutting {

struct Mp4Check {
    bool h26x = false;          ///< the video is H.264 or H.265
    NalCodec codec = NalCodec::H265;
    std::vector<Bytes> header_sets; ///< parameter sets in the header box
    std::vector<Bytes> stream_sets; ///< parameter sets in the first video packet
    bool tagged_hvc1 = false;

    /// The header holds what the stream needs: the stream's own parameter sets, or, if the
    /// stream has none (hvc1 files keep them only in the header), at least some.
    [[nodiscard]] bool header_usable() const {
        return stream_sets.empty() ? !header_sets.empty() : header_sets == stream_sets;
    }
    /// Nothing to repair: the header is usable, and H.265 is tagged hvc1.
    [[nodiscard]] bool fine() const {
        return !h26x || (header_usable() && (codec == NalCodec::H264 || tagged_hvc1));
    }
};

/// Reads the header box and the first video packet. Throws media::MediaError if the file
/// cannot be read.
[[nodiscard]] Mp4Check check_mp4(const std::filesystem::path& file);

enum class RepairOutcome {
    Repaired,
    AlreadyFine,
    NotRepairable, ///< not H.264/H.265, or no parameter sets in the stream to build from
};

/// Rewrites the file without re-encoding (packets copied, header box built from the stream's
/// parameter sets, H.265 tagged hvc1), next to it first; it replaces the original only after
/// the copy's header holds exactly the parameter sets of the original stream. Throws
/// media::MediaError if reading or writing fails.
[[nodiscard]] RepairOutcome repair_mp4(const std::filesystem::path& file);

} // namespace ttrally::cutting

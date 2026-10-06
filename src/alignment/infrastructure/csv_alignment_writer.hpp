// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/application/alignment_report.hpp"

#include <filesystem>

namespace ttrally::alignment {

/// Path of the gaps CSV that belongs to a segments CSV: "x.csv" -> "x.gaps.csv".
[[nodiscard]] std::filesystem::path gaps_path_for(const std::filesystem::path& segments_csv);

/// Writes the candidate segments (one row per segment of the cut video).
void write_segments_csv(const std::filesystem::path& path, const AlignmentReport& report);

/// Writes the gaps of the original.
void write_gaps_csv(const std::filesystem::path& path, const AlignmentReport& report);

} // namespace ttrally::alignment

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/application/alignment_report.hpp"

#include <filesystem>

namespace ttrally::alignment {

/// Writes the report as a single self-contained HTML file (inline CSS and SVG, no external
/// resources): summary, warnings, file properties, offset plot, segments and gaps.
void write_html_report(const std::filesystem::path& path, const AlignmentReport& report);

} // namespace ttrally::alignment

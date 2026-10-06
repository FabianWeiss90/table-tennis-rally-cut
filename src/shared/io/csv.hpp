// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::io {

using CsvRow = std::vector<std::string>;

/// Quotes a field if it contains a comma, quote or line break (RFC 4180).
[[nodiscard]] std::string csv_escape(std::string_view field);

/// Writes a CSV file with a header row. Line endings are "\n" on all platforms.
void write_csv(const std::filesystem::path& path, const CsvRow& header,
               const std::vector<CsvRow>& rows);

/// Reads a CSV file (supports quoted fields). Returns all rows including the header.
[[nodiscard]] std::vector<CsvRow> read_csv(const std::filesystem::path& path);

} // namespace ttrally::io

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/csv.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ttrally::io {

std::string csv_escape(std::string_view field) {
    if (field.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(field);
    }
    std::string quoted = "\"";
    for (const char c : field) {
        if (c == '"') {
            quoted += '"';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

void write_csv(const std::filesystem::path& path, const CsvRow& header,
               const std::vector<CsvRow>& rows) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    auto write_row = [&out](const CsvRow& row) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) {
                out << ',';
            }
            out << csv_escape(row[i]);
        }
        out << '\n';
    };
    write_row(header);
    for (const auto& row : rows) {
        write_row(row);
    }
    if (!out) {
        throw std::runtime_error("error while writing " + path.string());
    }
}

std::vector<CsvRow> read_csv(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    std::vector<CsvRow> rows;
    CsvRow row;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (c == '\n') {
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace ttrally::io

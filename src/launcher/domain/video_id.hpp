// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <string>
#include <string_view>

namespace ttrally::launcher {

/// Video id suggested for a file name without extension: lower case, words joined by "-",
/// trailing quality tags dropped ("Weiss-Heinemeier_FHD" -> "weiss-heinemeier").
[[nodiscard]] std::string suggest_video_id(std::string_view stem);

/// True if `candidate` names a version derived from `original`, such as a pre-cut
/// ("Match_FHD_cut_33_rallies" is derived from "Match_FHD").
[[nodiscard]] bool is_derived_from(std::string_view candidate, std::string_view original);

} // namespace ttrally::launcher

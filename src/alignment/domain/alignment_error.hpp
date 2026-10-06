// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdexcept>

namespace ttrally::alignment {

/// The two videos cannot be aligned (e.g. no audio, or the audio does not match).
class AlignmentNotPossible : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace ttrally::alignment

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Deliberate violation for the self-test of check_dependencies.cmake: a domain header must not
// depend on infrastructure or third-party libraries.

#pragma once

#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"

#include <pocketfft_hdronly.h>

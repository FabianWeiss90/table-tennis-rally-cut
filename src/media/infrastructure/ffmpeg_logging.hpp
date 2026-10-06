// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

namespace ttrally::media {

/// Limits FFmpeg's own console output to errors (verbose: warnings and info as well).
void configure_ffmpeg_logging(bool verbose);

} // namespace ttrally::media

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_logging.hpp"

#include "media/infrastructure/ffmpeg_api.hpp"

namespace ttrally::media {

void configure_ffmpeg_logging(bool verbose) {
    av_log_set_level(verbose ? AV_LOG_INFO : AV_LOG_ERROR);
}

} // namespace ttrally::media

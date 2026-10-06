// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_decode_backend_probe.hpp"

#include "media/infrastructure/ffmpeg_api.hpp"

namespace ttrally::media {

std::vector<BackendStatus> FfmpegDecodeBackendProbe::probe() {
    const ff::QuietLogScope quiet; // failures are expected and reported in the result
    std::vector<BackendStatus> statuses;
    for (const DecodeBackend backend : ff::platform_hardware_backends()) {
        BackendStatus status;
        status.backend = backend;
        const AVHWDeviceType type = ff::device_type(backend);
        if (type == AV_HWDEVICE_TYPE_NONE) {
            status.detail = "not supported by this FFmpeg build";
        } else {
            AVBufferRef* device = nullptr;
            const int result = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
            status.available = result >= 0;
            if (status.available) {
                av_buffer_unref(&device);
            } else {
                status.detail = ff::error_string(result);
            }
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

} // namespace ttrally::media

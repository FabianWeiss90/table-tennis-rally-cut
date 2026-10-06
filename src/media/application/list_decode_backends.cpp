// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/application/list_decode_backends.hpp"

namespace ttrally::media {

DecodeBackendOverview ListDecodeBackends::execute() {
    DecodeBackendOverview overview;
    overview.hardware = probe_.probe();
    overview.automatic = select_automatic_backend(overview.hardware);
    return overview;
}

} // namespace ttrally::media

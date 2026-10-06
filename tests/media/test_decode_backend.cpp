// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/application/list_decode_backends.hpp"
#include "media/domain/decode_backend.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ttrally::media;

namespace {

class FixedBackendProbe final : public DecodeBackendProbe {
  public:
    explicit FixedBackendProbe(std::vector<BackendStatus> statuses)
        : statuses_(std::move(statuses)) {}
    std::vector<BackendStatus> probe() override { return statuses_; }

  private:
    std::vector<BackendStatus> statuses_;
};

} // namespace

TEST_CASE("decode backend names round trip") {
    for (const auto& name : decode_backend_names()) {
        const auto backend = parse_decode_backend(name);
        REQUIRE(backend);
        CHECK(to_string(*backend) == name);
    }
    CHECK_FALSE(parse_decode_backend("opengl"));
}

TEST_CASE("auto selects the first available backend, else the CPU") {
    FixedBackendProbe probe({{DecodeBackend::Vaapi, false, "no device"},
                             {DecodeBackend::Cuda, true, ""},
                             {DecodeBackend::Vulkan, true, ""}});
    const auto overview = ListDecodeBackends(probe).execute();
    CHECK(overview.hardware.size() == 3);
    CHECK(overview.automatic == DecodeBackend::Cuda);

    FixedBackendProbe none({{DecodeBackend::Vaapi, false, "no device"}});
    CHECK(ListDecodeBackends(none).execute().automatic == DecodeBackend::Cpu);
}

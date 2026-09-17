#pragma once

#include <cstdint>

namespace dopecam::protocol {
constexpr std::uint16_t kDiscoveryPort = 39510;
constexpr std::uint16_t kControlPort = 39511;
constexpr std::uint16_t kVideoPort = 39512;
constexpr std::uint8_t kRtpPayloadType = 96;
constexpr char kDiscoverMessage[] = "DOPECAM_DISCOVER_V1";
}

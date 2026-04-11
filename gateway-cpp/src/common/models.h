#pragma once

#include <cstdint>
#include <string>

namespace cyrus {

// 占位：后续 /chat 与转发模型在此扩展。
struct GatewayConfigSnapshot {
    std::string listen_host{"0.0.0.0"};
    std::uint16_t listen_port{8080};
    std::string agent_base_url;
    std::int32_t agent_timeout_ms{8000};
    std::int32_t agent_retry_max{1};
};

}  // namespace cyrus

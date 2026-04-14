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
    std::int32_t rate_limit_capacity{500};
    std::int32_t rate_limit_refill_per_sec{200};
    std::int32_t sse_first_chunk_timeout_ms{5000};
    std::int32_t sse_total_timeout_ms{120000};
};

}  // namespace cyrus

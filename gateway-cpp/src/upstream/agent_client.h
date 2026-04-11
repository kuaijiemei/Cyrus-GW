#pragma once

#include "common/models.h"

#include <string>
#include <string_view>

namespace cyrus::upstream {

struct AgentClientResult {
    bool transport_ok{false};
    bool timed_out{false};
    int http_status{0};
    std::string http_body;
    std::string error;
};

AgentClientResult post_agent_chat(const GatewayConfigSnapshot& cfg, std::string_view request_json);

}  // namespace cyrus::upstream

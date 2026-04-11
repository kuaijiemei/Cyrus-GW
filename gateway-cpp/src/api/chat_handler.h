#pragma once

#include "common/models.h"

#include <string>

namespace cyrus::api {

struct ChatHttpResponse {
    int status_code{500};
    std::string json_body;
    std::string request_id;
    std::string tool_used;
    bool stream{false};
};

ChatHttpResponse handle_post_chat(const GatewayConfigSnapshot& cfg, const std::string& request_headers,
                                  const std::string& body);

}  // namespace cyrus::api

#pragma once

#include "common/models.h"

#include <functional>
#include <string>
#include <string_view>

namespace cyrus::api {

struct ChatHttpResponse {
    int status_code{500};
    std::string json_body;
    std::string request_id;
    std::string tool_used;
    bool stream{false};
    int ttft_ms{-1};
    int retry_count{0};
    std::string error_layer;
};

struct StreamCallbacks {
    std::function<bool(std::string_view)> write_chunk;
    std::function<void(int)> on_ttft_ms;
};

ChatHttpResponse handle_post_chat(const GatewayConfigSnapshot& cfg, const std::string& request_headers,
                                  const std::string& body, const StreamCallbacks* stream_callbacks = nullptr);

}  // namespace cyrus::api

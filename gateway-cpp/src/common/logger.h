#pragma once

#include <string>
#include <string_view>

namespace cyrus {

void log_startup(std::string_view message) noexcept;

// 单条请求访问日志（stderr，JSON 行）；字段对齐 log_fields / TECH_DESIGN。
void log_http_request(const std::string& request_id, int latency_ms, int status_code,
                      const std::string& tool_used, const std::string& path, bool stream,
                      int ttft_ms = -1, int retry_count = 0,
                      const std::string& error_layer = "",
                      int queue_wait_ms = -1) noexcept;

}  // namespace cyrus

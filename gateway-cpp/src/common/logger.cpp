#include "common/logger.h"

#include "common/json_util.h"
#include "common/log_fields.h"

#include <chrono>
#include <iostream>
#include <string_view>

namespace cyrus {

void log_startup(std::string_view message) noexcept {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
    std::cerr << "{\"event\":\"startup\",\"" << log_fields::kLatencyMs << "\":" << ms << ",\""
                        << log_fields::kStatusCode << "\":0,\"" << log_fields::kToolUsed << "\":\"\","
                        << "\"message\":\"" << json_escape(message) << "\"}\n";
}

void log_http_request(const std::string& request_id, int latency_ms, int status_code,
                                            const std::string& tool_used, const std::string& path, bool stream) noexcept {
    try {
        std::cerr << "{\"" << log_fields::kRequestId << "\":\"" << json_escape(request_id) << "\",\""
                            << log_fields::kLatencyMs << "\":" << latency_ms << ",\"" << log_fields::kStatusCode << "\":"
                            << status_code << ",\"" << log_fields::kToolUsed << "\":\"" << json_escape(tool_used) << "\",\""
                            << log_fields::kPath << "\":\"" << json_escape(path) << "\",\"" << log_fields::kStream << "\":"
                            << (stream ? "true" : "false") << "}\n";
    } catch (...) {
    }
}

}  // namespace cyrus

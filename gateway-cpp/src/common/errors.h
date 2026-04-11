#pragma once

#include <cstdint>
#include <string_view>

namespace cyrus {

// 统一错误码初稿：Gateway 1xxxx，Agent 2xxxx，LLM/上游 3xxxx（便于日志 error_code 对齐）。
enum class ErrorCode : std::uint32_t {
    kOk = 0,

    kGwInternal = 10001,
    kGwBadRequest = 10002,
    kGwRateLimited = 10003,
    kGwUpstreamUnavailable = 10004,
    kGwUpstreamTimeout = 10005,

    kAgInternal = 20001,
    kAgBadRequest = 20002,
    kAgLlmError = 20003,
    kAgToolError = 20004,

    kLlmTimeout = 30001,
    kLlmBadResponse = 30002,
};

constexpr std::string_view error_code_name(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::kOk:
            return "OK";
        case ErrorCode::kGwInternal:
            return "GW_INTERNAL";
        case ErrorCode::kGwBadRequest:
            return "GW_BAD_REQUEST";
        case ErrorCode::kGwRateLimited:
            return "GW_RATE_LIMITED";
        case ErrorCode::kGwUpstreamUnavailable:
            return "GW_UPSTREAM_UNAVAILABLE";
        case ErrorCode::kGwUpstreamTimeout:
            return "GW_UPSTREAM_TIMEOUT";
        case ErrorCode::kAgInternal:
            return "AG_INTERNAL";
        case ErrorCode::kAgBadRequest:
            return "AG_BAD_REQUEST";
        case ErrorCode::kAgLlmError:
            return "AG_LLM_ERROR";
        case ErrorCode::kAgToolError:
            return "AG_TOOL_ERROR";
        case ErrorCode::kLlmTimeout:
            return "LLM_TIMEOUT";
        case ErrorCode::kLlmBadResponse:
            return "LLM_BAD_RESPONSE";
    }
    return "UNKNOWN";
}

}  // namespace cyrus

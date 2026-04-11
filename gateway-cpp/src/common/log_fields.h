#pragma once

// 与 configs/logging_fields.yaml 对齐的 JSON 字段名（初稿）。
namespace cyrus::log_fields {

inline constexpr char kRequestId[] = "request_id";
inline constexpr char kLatencyMs[] = "latency_ms";
inline constexpr char kStatusCode[] = "status_code";
inline constexpr char kToolUsed[] = "tool_used";
inline constexpr char kPath[] = "path";
inline constexpr char kQueueWaitMs[] = "queue_wait_ms";
inline constexpr char kLlmCallLatencyMs[] = "llm_call_latency_ms";
inline constexpr char kStream[] = "stream";
inline constexpr char kRetryCount[] = "retry_count";
inline constexpr char kClientIp[] = "client_ip";
inline constexpr char kErrorCode[] = "error_code";
inline constexpr char kErrorLayer[] = "error_layer";

}  // namespace cyrus::log_fields

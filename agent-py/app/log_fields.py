"""与 gateway-cpp/configs/logging_fields.yaml 对齐的字段名常量（初稿）。"""

REQUEST_ID = "request_id"
LATENCY_MS = "latency_ms"
STATUS_CODE = "status_code"
TOOL_USED = "tool_used"
PATH = "path"
QUEUE_WAIT_MS = "queue_wait_ms"
LLM_CALL_LATENCY_MS = "llm_call_latency_ms"
STREAM = "stream"
RETRY_COUNT = "retry_count"
CLIENT_IP = "client_ip"
ERROR_CODE = "error_code"
ERROR_LAYER = "error_layer"

REQUIRED_LOG_FIELDS = (
    REQUEST_ID,
    LATENCY_MS,
    STATUS_CODE,
    TOOL_USED,
)

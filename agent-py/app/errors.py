"""与 gateway-cpp/src/common/errors.h 对齐的统一错误码初稿（IntEnum）。"""

from enum import IntEnum


class ErrorCode(IntEnum):
    OK = 0

    GW_INTERNAL = 10001
    GW_BAD_REQUEST = 10002
    GW_RATE_LIMITED = 10003
    GW_UPSTREAM_UNAVAILABLE = 10004
    GW_UPSTREAM_TIMEOUT = 10005

    AG_INTERNAL = 20001
    AG_BAD_REQUEST = 20002
    AG_LLM_ERROR = 20003
    AG_TOOL_ERROR = 20004

    LLM_TIMEOUT = 30001
    LLM_BAD_RESPONSE = 30002

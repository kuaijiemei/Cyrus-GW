"""
模块职责：定义 Tool 框架统一接口协议（输入/输出/错误码）。

对外暴露：
- ToolInvokeRequest: Tool 调用输入模型
- ToolInvokeResponse: Tool 执行输出模型（包含成功与错误分支）
- ToolErrorCode: Tool 层标准错误码枚举

关键分支原因：
- error_code 使用枚举字符串而非整数：便于日志与脚本断言直接阅读，不需要码表转换
- ok + error_code 双字段：调用方可先看 ok 快速分支，再按 error_code 精细处理

易踩坑点：
- request_id 必须透传到 Tool 层日志，避免跨 Gateway/Agent/tool 排障断链
- tool_args 必须为 dict；若上游传 list/str，应在路由层转为 TOOL_BAD_ARGS
- error_message 返回前需保持可 JSON 序列化文本，避免异常对象直传导致序列化失败
"""

from enum import Enum
from typing import Any, Dict

from pydantic import BaseModel, Field


class ToolErrorCode(str, Enum):
    """Tool 框架统一错误码。"""

    NONE = "none"
    TOOL_NOT_FOUND = "tool_not_found"
    TOOL_BAD_ARGS = "tool_bad_args"
    TOOL_RUNTIME_ERROR = "tool_runtime_error"


class ToolInvokeRequest(BaseModel):
    """Tool 调用请求协议。"""

    request_id: str = Field(..., description="链路请求 ID，必须与 Agent 请求保持一致")
    tool_name: str = Field(..., description="要执行的工具名，如 time_tool / echo_tool")
    tool_args: Dict[str, Any] = Field(default_factory=dict, description="工具入参字典")


class ToolInvokeResponse(BaseModel):
    """Tool 调用响应协议。"""

    request_id: str
    tool_name: str
    ok: bool
    output: str = ""
    error_code: ToolErrorCode = ToolErrorCode.NONE
    error_message: str = ""

"""
模块职责：所有 Agent 对外 / 对内的 Pydantic 数据模型。

关键分支原因：
- DecisionAction 使用 str 枚举：JSON 序列化直接输出字符串，方便日志肉眼阅读
- DecisionModel.tool_args 默认空 dict：避免 LLM 漏填字段时 None 解包崩溃

易踩坑点：
- Optional[str] 字段在 Pydantic v2 序列化时会输出 null，下游需要 or "" 防御
- field_validator 在 v2 必须加 @classmethod；v1 写法会静默跳过校验
"""

from enum import Enum
from typing import Any, Dict, Optional

from pydantic import BaseModel, Field, field_validator


# ── 健康检查 ───────────────────────────────────────────────────────────────────

class HealthResponse(BaseModel):
    status: str = Field(default="ok")
    service: str = Field(default="agent")


# ── 请求/响应主模型 ────────────────────────────────────────────────────────────

class AgentChatRequest(BaseModel):
    request_id: Optional[str] = None
    message: str = Field(..., description="User prompt text")
    stream: bool = Field(default=False)
    session_id: Optional[str] = None

    @field_validator("message")
    @classmethod
    def validate_message(cls, value: str) -> str:
        if not value or not value.strip():
            raise ValueError("message must not be empty")
        return value


class AgentChatResponse(BaseModel):
    request_id: str
    answer: str
    tool_used: str = ""
    model: str
    retry_count: int = 0


class AgentErrorResponse(BaseModel):
    request_id: str
    error: str
    error_code: int
    detail: Optional[str] = None


# ── 决策模型（§3.1 Agent 决策机制）──────────────────────────────────────────────

class DecisionAction(str, Enum):
    """Agent 决策动作枚举，str 枚举确保 JSON 序列化可读。"""
    DIRECT_ANSWER = "direct_answer"
    TOOL_CALL = "tool_call"


class DecisionModel(BaseModel):
    """
    Agent 决策结构，由规则优先或 LLM 辅助填充。

    字段语义：
    - action: 动作类型，必填
    - tool_name: 工具名称，仅 action=tool_call 时有效；null 表示直接回答
    - tool_args: 工具调用参数，key/value 均为字符串兼容类型；无参数时为空 dict

    设计取舍：tool_args 使用 Dict[str, Any] 而非具体模型，因为工具参数结构各异，
    MVP 阶段由工具函数签名隐式约束，不做额外 schema 校验。
    """
    action: DecisionAction = DecisionAction.DIRECT_ANSWER
    tool_name: Optional[str] = None
    tool_args: Dict[str, Any] = Field(default_factory=dict)

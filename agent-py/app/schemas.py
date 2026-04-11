"""Pydantic schemas for Agent API (TODO 1.3)."""

from typing import Optional

from pydantic import BaseModel, Field, field_validator


class HealthResponse(BaseModel):
    status: str = Field(default="ok")
    service: str = Field(default="agent")


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


class AgentErrorResponse(BaseModel):
    request_id: str
    error: str
    error_code: int
    detail: Optional[str] = None

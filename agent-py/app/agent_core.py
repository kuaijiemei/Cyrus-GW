"""Agent core orchestration for TODO 1.3."""

import uuid
from typing import AsyncIterator

from app.llm_client import LLMResult, OpenAICompatibleLLMClient
from app.schemas import AgentChatRequest, AgentChatResponse


class AgentCore:
    def __init__(self, llm_client: OpenAICompatibleLLMClient) -> None:
        self._llm_client = llm_client

    async def handle_chat(self, req: AgentChatRequest) -> AgentChatResponse:
        request_id = req.request_id or f"req_{uuid.uuid4().hex[:16]}"
        llm_result: LLMResult = await self._llm_client.generate(req.message)
        return AgentChatResponse(
            request_id=request_id,
            answer=llm_result.answer,
            tool_used="",
            model=llm_result.model,
            retry_count=llm_result.retry_count,
        )

    async def handle_chat_stream(self, req: AgentChatRequest) -> AsyncIterator[str]:
        async for delta in self._llm_client.generate_stream(req.message):
            yield delta

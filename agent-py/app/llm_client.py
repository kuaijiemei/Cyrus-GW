"""Minimal OpenAI-compatible LLM client with timeout control."""

from dataclasses import dataclass
from typing import Any, Dict

import httpx

from config import AgentSettings


@dataclass
class LLMResult:
    answer: str
    model: str


class LLMClientError(RuntimeError):
    pass


class LLMConfigError(LLMClientError):
    pass


class LLMTimeoutError(LLMClientError):
    pass


class LLMUpstreamError(LLMClientError):
    pass


class OpenAICompatibleLLMClient:
    """Single-provider client for Chat Completions endpoint."""

    def __init__(self, settings: AgentSettings) -> None:
        self._settings = settings

    async def generate(self, message: str) -> LLMResult:
        if not self._settings.llm_base_url:
            raise LLMConfigError("LLM_BASE_URL is not configured")
        if not self._settings.llm_api_key:
            raise LLMConfigError("LLM_API_KEY is not configured")

        base = self._settings.llm_base_url.rstrip("/")
        url = f"{base}/chat/completions"
        payload: Dict[str, Any] = {
            "model": self._settings.llm_model,
            "messages": [
                {"role": "system", "content": "You are Cyrus-GW agent."},
                {"role": "user", "content": message},
            ],
            "temperature": 0.2,
        }
        headers = {
            "Authorization": f"Bearer {self._settings.llm_api_key}",
            "Content-Type": "application/json",
        }

        timeout_s = max(self._settings.llm_timeout_ms, 1) / 1000.0
        timeout = httpx.Timeout(timeout=timeout_s)

        try:
            async with httpx.AsyncClient(timeout=timeout) as client:
                resp = await client.post(url, json=payload, headers=headers)
            resp.raise_for_status()
        except httpx.TimeoutException as exc:
            raise LLMTimeoutError(
                f"LLM request timed out after {self._settings.llm_timeout_ms} ms"
            ) from exc
        except httpx.HTTPStatusError as exc:
            status = exc.response.status_code
            raise LLMUpstreamError(f"LLM upstream returned HTTP {status}") from exc
        except httpx.HTTPError as exc:
            raise LLMUpstreamError(f"LLM upstream request failed: {exc}") from exc

        data = resp.json()
        choices = data.get("choices")
        if not choices:
            raise LLMUpstreamError("LLM upstream returned empty choices")

        content = choices[0].get("message", {}).get("content", "")
        if not isinstance(content, str) or not content.strip():
            raise LLMUpstreamError("LLM upstream returned empty content")

        model = str(data.get("model") or self._settings.llm_model)
        return LLMResult(answer=content.strip(), model=model)

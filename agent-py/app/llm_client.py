"""Minimal OpenAI-compatible LLM client with timeout and stream support."""

import json
from dataclasses import dataclass
from typing import Any, AsyncIterator, Dict

import httpx

from config import AgentSettings


@dataclass
class LLMResult:
    answer: str
    model: str
    retry_count: int = 0


class LLMClientError(RuntimeError):
    pass


class LLMConfigError(LLMClientError):
    pass


class LLMTimeoutError(LLMClientError):
    def __init__(self, message: str, retry_count: int = 0) -> None:
        super().__init__(message)
        self.retry_count = retry_count


class LLMUpstreamError(LLMClientError):
    pass


class OpenAICompatibleLLMClient:
    """Single-provider client for Chat Completions endpoint."""

    def __init__(self, settings: AgentSettings) -> None:
        self._settings = settings

    def _build_base_request(self, message: str) -> tuple[str, Dict[str, Any], Dict[str, str], httpx.Timeout]:
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
        return url, payload, headers, timeout

    def _retry_limit(self) -> int:
        return max(0, min(int(self._settings.llm_retry_max), 1))

    async def generate(self, message: str) -> LLMResult:
        url, payload, headers, timeout = self._build_base_request(message)
        retry_limit = self._retry_limit()
        attempt = 0
        while True:
            try:
                async with httpx.AsyncClient(timeout=timeout) as client:
                    resp = await client.post(url, json=payload, headers=headers)
                resp.raise_for_status()
                break
            except httpx.TimeoutException as exc:
                if attempt >= retry_limit:
                    raise LLMTimeoutError(
                        f"LLM request timed out after {self._settings.llm_timeout_ms} ms",
                        retry_count=attempt,
                    ) from exc
                attempt += 1
                continue
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
        return LLMResult(answer=content.strip(), model=model, retry_count=attempt)

    async def generate_stream(self, message: str) -> AsyncIterator[str]:
        retry_limit = self._retry_limit()
        attempt = 0
        while True:
            url, payload, headers, timeout = self._build_base_request(message)
            payload["stream"] = True
            saw_any_chunk = False
            try:
                async with httpx.AsyncClient(timeout=timeout) as client:
                    async with client.stream("POST", url, json=payload, headers=headers) as resp:
                        resp.raise_for_status()
                        async for line in resp.aiter_lines():
                            if not line:
                                continue
                            if not line.startswith("data:"):
                                continue
                            data_str = line[5:].strip()
                            if not data_str:
                                continue
                            if data_str == "[DONE]":
                                break
                            try:
                                data = json.loads(data_str)
                            except json.JSONDecodeError:
                                continue
                            choices = data.get("choices") or []
                            if not choices:
                                continue
                            delta = choices[0].get("delta", {})
                            text = delta.get("content")
                            if isinstance(text, str) and text:
                                saw_any_chunk = True
                                yield text
                if not saw_any_chunk:
                    raise LLMUpstreamError("LLM upstream returned empty stream")
                return
            except httpx.TimeoutException as exc:
                if saw_any_chunk or attempt >= retry_limit:
                    raise LLMTimeoutError(
                        f"LLM stream timed out after {self._settings.llm_timeout_ms} ms",
                        retry_count=attempt,
                    ) from exc
                attempt += 1
                continue
            except httpx.HTTPStatusError as exc:
                status = exc.response.status_code
                raise LLMUpstreamError(f"LLM upstream returned HTTP {status}") from exc
            except httpx.HTTPError as exc:
                raise LLMUpstreamError(f"LLM upstream stream failed: {exc}") from exc

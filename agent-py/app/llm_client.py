"""
模块职责：OpenAI-compatible LLM 客户端，封装超时、重试、错误分类。

关键分支原因：
- LLMConfigError → 503：依赖未就绪（环境变量缺失），区别于「LLM 挂了」的 502
- LLMTimeoutError → 504：等待超时，重试 1 次后仍超即报；retry_count 可观测
- LLMUpstreamError → 502：HTTP 非 2xx 或网络异常，属于「坏上游」语义
- generate_with_messages：agent_core 决策步骤需要传自定义 messages（含
  system 决策 prompt），不能复用 generate(message) 的固定 system 消息

易踩坑点：
- generate_stream 首包超时与总超时共用同一个 httpx.Timeout(timeout=N)，
  两者语义不同但实现合并；MVP 阶段不做拆分
- 流式场景已收到 chunk 后再超时，不触发重试（saw_any_chunk guard），
  避免重复 chunk 导致客户端内容乱序
- LLM 决策调用（generate_with_messages）使用低温（temperature=0.0）确保
  JSON 输出确定性；调用方传 temperature 参数，默认 0.2 与历史行为一致
- generate_stream_with_messages 与 generate_stream 共享同一套重试逻辑，
  通过内部 _stream_messages 辅助方法消除重复
"""

import json
import logging
from dataclasses import dataclass
from typing import Any, AsyncIterator, Dict, List

import httpx

from config import AgentSettings

logger = logging.getLogger(__name__)


# ── 异常类型 ───────────────────────────────────────────────────────────────────

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


# ── 数据类 ─────────────────────────────────────────────────────────────────────

@dataclass
class LLMResult:
    answer: str
    model: str
    retry_count: int = 0


# ── 客户端 ─────────────────────────────────────────────────────────────────────

class OpenAICompatibleLLMClient:
    """单 provider Chat Completions 客户端，支持非流式与流式两种调用模式。"""

    def __init__(self, settings: AgentSettings) -> None:
        self._settings = settings

    # ── 内部辅助 ───────────────────────────────────────────────────────────────

    def _build_connection_params(self) -> tuple:
        """构造 URL、请求头和 timeout；不含 messages，方便复用。"""
        if not self._settings.llm_base_url:
            raise LLMConfigError("LLM_BASE_URL is not configured")
        if not self._settings.llm_api_key:
            raise LLMConfigError("LLM_API_KEY is not configured")
        base = self._settings.llm_base_url.rstrip("/")
        url = f"{base}/chat/completions"
        headers: Dict[str, str] = {
            "Authorization": f"Bearer {self._settings.llm_api_key}",
            "Content-Type": "application/json",
        }
        timeout_s = max(self._settings.llm_timeout_ms, 1) / 1000.0
        timeout = httpx.Timeout(timeout=timeout_s)
        return url, headers, timeout

    def _build_payload(self, messages: List[Dict[str, Any]], temperature: float = 0.2) -> Dict[str, Any]:
        return {
            "model": self._settings.llm_model,
            "messages": messages,
            "temperature": temperature,
        }

    def _retry_limit(self) -> int:
        return max(0, min(int(self._settings.llm_retry_max), 1))

    def _default_messages(self, message: str) -> List[Dict[str, Any]]:
        return [
            {"role": "system", "content": "You are Cyrus-GW agent."},
            {"role": "user", "content": message},
        ]

    # ── 非流式接口 ─────────────────────────────────────────────────────────────

    async def generate(self, message: str) -> LLMResult:
        """便捷接口：固定 system prompt，适用于普通对话路径。"""
        return await self.generate_with_messages(self._default_messages(message))

    async def generate_with_messages(
        self,
        messages: List[Dict[str, Any]],
        temperature: float = 0.2,
    ) -> LLMResult:
        """
        通用非流式接口，调用方可完全控制 messages 列表。

        用于：
        - agent_core 决策步骤（系统 prompt 为路由指令，temperature=0.0）
        - agent_core 最终回答步骤（带 tool 结果的上下文注入）
        """
        url, headers, timeout = self._build_connection_params()
        payload = self._build_payload(messages, temperature)
        retry_limit = self._retry_limit()
        attempt = 0
        resp: httpx.Response

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

    # ── 流式接口 ───────────────────────────────────────────────────────────────

    async def generate_stream(self, message: str) -> AsyncIterator[str]:
        """便捷接口：固定 system prompt，适用于普通流式对话路径。"""
        async for delta in self.generate_stream_with_messages(self._default_messages(message)):
            yield delta

    async def generate_stream_with_messages(
        self,
        messages: List[Dict[str, Any]],
        temperature: float = 0.2,
    ) -> AsyncIterator[str]:
        """
        通用流式接口，调用方可完全控制 messages 列表。

        用于：agent_core 在决策/tool 执行后，以完整上下文（含 tool 结果）流式生成最终回答。

        重试语义：仅在 saw_any_chunk=False（首包前）且 attempt < retry_limit 时重试；
        已收到 chunk 后超时不重试，避免下游重复 chunk 问题。
        """
        retry_limit = self._retry_limit()
        attempt = 0

        while True:
            url, headers, timeout = self._build_connection_params()
            payload = self._build_payload(messages, temperature)
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

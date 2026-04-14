"""
模块职责：对外 HTTP 路由（/agent/chat），Gateway 之后的业务入口。

职责：调用 AgentCore；将 LLM 配置缺失、超时、上游错误转为明确 HTTP 状态与 JSON，
避免未捕获异常变成 500 黑盒。stderr 结构化日志字段与网关 access 对齐，便于按 request_id 串联。

关键分支原因：
- 503（LLMConfigError）：依赖未就绪，区别于「LLM 挂了」的 502，运维先查环境变量
- 504（LLMTimeoutError）：LLM 侧等待超限，与网关等 Agent 超时同属「超时」族
- 502（LLMUpstreamError）：HTTP 非 2xx 或网络类失败，属「坏上游」语义
- 流式路径的 tool_used：由 handle_chat_stream_with_tool_info 在 plan 阶段确定，
  SSE 结束时用正确 tool_used 打日志，而不是一律写空串

易踩坑点：
- request_id 透传：从 req.request_id 拿，若为 None 则在这里生成，保证全链路一致
- SSE 已发头（200）后，plan/stream 内的错误只能以 SSE error event 形式告知客户端
- _log_access 的 tool_used 参数在流式路径中通过 plan() 提前获取，避免 SSE 闭包捕获旧值
"""

import json
import sys
import time
import uuid
from typing import AsyncIterator

from fastapi import APIRouter
from fastapi.responses import JSONResponse, StreamingResponse

from app.agent_core import AgentCore
from app.errors import ErrorCode
from app.llm_client import LLMConfigError, LLMTimeoutError, LLMUpstreamError, OpenAICompatibleLLMClient
from app.log_fields import LATENCY_MS, PATH, REQUEST_ID, RETRY_COUNT, STATUS_CODE, STREAM, TOOL_USED
from app.schemas import AgentChatRequest
from config import get_settings

router = APIRouter()

_settings = get_settings()
_core = AgentCore(OpenAICompatibleLLMClient(_settings))


def _log_access(
    request_id: str,
    latency_ms: int,
    status_code: int,
    tool_used: str,
    path: str,
    stream: bool,
    retry_count: int = 0,
    error_layer: str = "",
) -> None:
    payload = {
        REQUEST_ID: request_id,
        LATENCY_MS: latency_ms,
        STATUS_CODE: status_code,
        TOOL_USED: tool_used,
        PATH: path,
        STREAM: stream,
        RETRY_COUNT: retry_count,
    }
    if error_layer:
        payload["error_layer"] = error_layer
    print(json.dumps(payload, ensure_ascii=True), file=sys.stderr, flush=True)


def _sse_event(event_name: str, payload: dict) -> str:
    return f"event: {event_name}\ndata: {json.dumps(payload, ensure_ascii=True)}\n\n"


def _request_id_or_new(req: AgentChatRequest) -> str:
    return req.request_id or f"req_{uuid.uuid4().hex[:16]}"


@router.post("/agent/chat")
async def agent_chat(req: AgentChatRequest):
    started = time.perf_counter()
    request_id = _request_id_or_new(req)
    req = req.model_copy(update={"request_id": request_id})

    if req.stream:
        async def sse_iter() -> AsyncIterator[str]:
            # tool_used 在 plan 阶段确定，SSE 结束时用于日志记录
            tool_used = ""
            first_delta_sent = False
            stream_failed = False

            try:
                # plan 阶段（决策 + tool 执行）：在 SSE 第一个 chunk 发出前完成
                # 若 plan 失败，异常在此捕获并转为 SSE error event
                tool_used, delta_gen = await _core.handle_chat_stream_with_tool_info(req)

                async for delta in delta_gen:
                    first_delta_sent = True
                    yield _sse_event("delta", {"request_id": request_id, "delta": delta})

                yield _sse_event("done", {"request_id": request_id})
                latency_ms = int((time.perf_counter() - started) * 1000)
                _log_access(request_id, latency_ms, 200, tool_used, "/agent/chat", True, 0, "")

            except LLMConfigError as exc:
                stream_failed = True
                latency_ms = int((time.perf_counter() - started) * 1000)
                _log_access(request_id, latency_ms, 503, tool_used, "/agent/chat", True, 0, "llm")
                yield _sse_event(
                    "error",
                    {
                        "request_id": request_id,
                        "error": "llm_not_configured",
                        "error_code": int(ErrorCode.AG_LLM_ERROR),
                        "detail": str(exc),
                    },
                )
            except LLMTimeoutError as exc:
                stream_failed = True
                latency_ms = int((time.perf_counter() - started) * 1000)
                _log_access(
                    request_id, latency_ms, 504, tool_used, "/agent/chat",
                    True, getattr(exc, "retry_count", 0), "llm",
                )
                yield _sse_event(
                    "error",
                    {
                        "request_id": request_id,
                        "error": "llm_timeout",
                        "error_code": int(ErrorCode.LLM_TIMEOUT),
                        "detail": str(exc),
                    },
                )
            except LLMUpstreamError as exc:
                stream_failed = True
                latency_ms = int((time.perf_counter() - started) * 1000)
                _log_access(request_id, latency_ms, 502, tool_used, "/agent/chat", True, 0, "llm")
                yield _sse_event(
                    "error",
                    {
                        "request_id": request_id,
                        "error": "llm_upstream_error",
                        "error_code": int(ErrorCode.AG_LLM_ERROR),
                        "detail": str(exc),
                    },
                )

            # 上游返回空流同样视作错误事件，避免网关卡住等待 done
            if not first_delta_sent and not stream_failed:
                yield _sse_event(
                    "error",
                    {
                        "request_id": request_id,
                        "error": "empty_stream",
                        "error_code": int(ErrorCode.AG_LLM_ERROR),
                        "detail": "no delta received before stream ended",
                    },
                )
                latency_ms = int((time.perf_counter() - started) * 1000)
                _log_access(request_id, latency_ms, 502, tool_used, "/agent/chat", True, 0, "llm")

        return StreamingResponse(sse_iter(), media_type="text/event-stream")

    # ── 非流式路径 ─────────────────────────────────────────────────────────────
    try:
        resp = await _core.handle_chat(req)
        latency_ms = int((time.perf_counter() - started) * 1000)
        # 成功路径用 core 回填的 request_id 打日志：与网关透传值一致，避免空串导致链路断档
        _log_access(
            resp.request_id, latency_ms, 200, resp.tool_used,
            "/agent/chat", req.stream, getattr(resp, "retry_count", 0), "",
        )
        return resp
    # 503：依赖未就绪，区别于「LLM 挂了」的 502，运维先查环境变量而非查模型厂商
    except LLMConfigError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 503
        _log_access(request_id, latency_ms, status, "", "/agent/chat", req.stream, 0, "llm")
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_not_configured",
                "error_code": int(ErrorCode.AG_LLM_ERROR),
                "detail": str(exc),
            },
        )
    # 504：LLM 侧等待超限，与网关等 Agent 超时可同属「超时」族，客户端可重试策略一致
    except LLMTimeoutError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 504
        _log_access(
            request_id, latency_ms, status, "", "/agent/chat",
            req.stream, getattr(exc, "retry_count", 0), "llm",
        )
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_timeout",
                "error_code": int(ErrorCode.LLM_TIMEOUT),
                "detail": str(exc),
            },
        )
    # 502：HTTP 非 2xx 或网络类失败，语义为坏上游；不把厂商状态码原样暴露为 200
    except LLMUpstreamError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 502
        _log_access(request_id, latency_ms, status, "", "/agent/chat", req.stream, 0, "llm")
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_upstream_error",
                "error_code": int(ErrorCode.AG_LLM_ERROR),
                "detail": str(exc),
            },
        )

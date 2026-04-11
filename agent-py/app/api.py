"""HTTP routes for agent service."""

import json
import sys
import time

from fastapi import APIRouter
from fastapi.responses import JSONResponse

from app.agent_core import AgentCore
from app.errors import ErrorCode
from app.llm_client import LLMConfigError, LLMTimeoutError, LLMUpstreamError, OpenAICompatibleLLMClient
from app.log_fields import LATENCY_MS, PATH, REQUEST_ID, STATUS_CODE, STREAM, TOOL_USED
from app.schemas import AgentChatRequest, AgentChatResponse
from config import get_settings

router = APIRouter()

_settings = get_settings()
_core = AgentCore(OpenAICompatibleLLMClient(_settings))


def _log_access(request_id: str, latency_ms: int, status_code: int, tool_used: str, path: str, stream: bool) -> None:
    payload = {
        REQUEST_ID: request_id,
        LATENCY_MS: latency_ms,
        STATUS_CODE: status_code,
        TOOL_USED: tool_used,
        PATH: path,
        STREAM: stream,
    }
    print(json.dumps(payload, ensure_ascii=True), file=sys.stderr, flush=True)


@router.post("/agent/chat", response_model=AgentChatResponse)
async def agent_chat(req: AgentChatRequest):
    started = time.perf_counter()
    request_id = req.request_id or ""
    try:
        resp = await _core.handle_chat(req)
        latency_ms = int((time.perf_counter() - started) * 1000)
        _log_access(resp.request_id, latency_ms, 200, resp.tool_used, "/agent/chat", req.stream)
        return resp
    except LLMConfigError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 503
        request_id = request_id or "req_unknown"
        _log_access(request_id, latency_ms, status, "", "/agent/chat", req.stream)
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_not_configured",
                "error_code": int(ErrorCode.AG_LLM_ERROR),
                "detail": str(exc),
            },
        )
    except LLMTimeoutError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 504
        request_id = request_id or "req_unknown"
        _log_access(request_id, latency_ms, status, "", "/agent/chat", req.stream)
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_timeout",
                "error_code": int(ErrorCode.LLM_TIMEOUT),
                "detail": str(exc),
            },
        )
    except LLMUpstreamError as exc:
        latency_ms = int((time.perf_counter() - started) * 1000)
        status = 502
        request_id = request_id or "req_unknown"
        _log_access(request_id, latency_ms, status, "", "/agent/chat", req.stream)
        return JSONResponse(
            status_code=status,
            content={
                "request_id": request_id,
                "error": "llm_upstream_error",
                "error_code": int(ErrorCode.AG_LLM_ERROR),
                "detail": str(exc),
            },
        )

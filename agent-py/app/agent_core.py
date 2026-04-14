"""
模块职责：Agent 决策与执行层。接收 AgentChatRequest，
按「规则优先 → LLM 辅助 → 兜底」三级顺序决策 action，
执行 tool（若有），再将 tool 结果注入上下文生成最终回答。

关键分支原因：
- 规则优先（_rule_based_decision）：已知关键词场景无需 LLM 调用，
  减少 1 次 LLM round-trip 约 200-800ms 延迟
- LLM 辅助（_llm_assisted_decision）：处理复杂/多义请求，
  使用 temperature=0.0 保证决策输出稳定性；JSON 解析失败直接兜底 direct_answer
- tool 执行失败降级：ToolNotFoundError / ToolExecutionError 均捕获，
  决策降级为 direct_answer，保证请求不因 tool 异常崩溃
- plan() 分离：将决策+tool 执行从最终 LLM 调用分离，
  让 api.py 在 SSE 场景可在流式开始前获取 tool_used 用于日志

易踩坑点：
- request_id 透传：必须用 req.request_id，不能在 agent_core 内重新 uuid4，
  否则网关日志与 Agent 日志的 request_id 不一致，跨层追踪断链
- LLM 决策 response 可能含 markdown 包裹（```json...```）或前缀文字，
  _extract_json() 用正则提取第一个 {...} 块再解析
- echo_tool 的 tool_args 必须含 {"text": "..."} key，LLM 决策或规则层
  需确保字段名与 echo_tool.run(text) 签名一致
- 流式路径：_plan() 是普通 coroutine（await），在 SSE generator 内等待，
  plan 失败会触发 SSE error event 而非 HTTP 4xx（StreamingResponse 已发头）
"""

import json
import logging
import re
import uuid
from typing import AsyncIterator, Dict, List, Optional, Tuple

from app.llm_client import LLMResult, OpenAICompatibleLLMClient
from app.schemas import AgentChatRequest, AgentChatResponse, DecisionAction, DecisionModel
from app.tool_router import ToolRouter
from app.tools.contracts import ToolInvokeRequest

logger = logging.getLogger(__name__)


# ── 决策 system prompt ──────────────────────────────────────────────────────────

_DECISION_SYSTEM_PROMPT = """\
You are a routing agent for Cyrus-GW. Given a user message, decide the action.
Return ONLY valid JSON with no extra text, no markdown, no explanation:
{"action": "direct_answer"|"tool_call", "tool_name": "time_tool"|"echo_tool"|null, "tool_args": {}}

Decision rules:
- User asks about current time / date / clock / what time is it
  → {"action": "tool_call", "tool_name": "time_tool", "tool_args": {}}
- User asks to echo / repeat / parrot specific text
  → {"action": "tool_call", "tool_name": "echo_tool", "tool_args": {"text": "<text to repeat>"}}
- Otherwise
  → {"action": "direct_answer", "tool_name": null, "tool_args": {}}
"""

# 规则关键词（中英文），命中即走规则路径，无需 LLM 决策
_TIME_KEYWORDS: frozenset = frozenset([
    "现在几点", "几点了", "现在时间", "当前时间", "今天几号", "今天日期",
    "现在是几点", "what time is it", "current time", "what's the time",
])
_ECHO_PREFIXES: Tuple[str, ...] = ("echo ", "请回显 ", "repeat: ", "parrot: ")


# ── 内部工具函数 ────────────────────────────────────────────────────────────────

def _extract_json(text: str) -> str:
    """
    从 LLM 输出中提取第一个 {...} JSON 块。

    处理场景：
    - 纯 JSON：直接返回
    - markdown 包裹（```json...```）：剥除反引号
    - 前缀文字（"Sure! {..."）：定位第一个 { 和最后一个 }
    """
    text = text.strip()
    # 剥除 markdown 代码块
    text = re.sub(r"```(?:json)?\s*", "", text, flags=re.IGNORECASE)
    text = text.strip("`").strip()
    # 提取第一个 { 到最后一个 }
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end >= start:
        return text[start : end + 1]
    return text


# ── AgentCore ──────────────────────────────────────────────────────────────────

class AgentCore:
    """
    Agent 核心编排器，对外暴露 handle_chat / handle_chat_stream_with_tool_info。

    职责边界：只做决策路由与执行编排，不直接处理 HTTP / SSE 协议细节（那是 api.py 的责任）。
    """

    def __init__(self, llm_client: OpenAICompatibleLLMClient) -> None:
        self._llm_client = llm_client
        self._tool_router = ToolRouter()

    # ── 决策（三级）────────────────────────────────────────────────────────────

    def _rule_based_decision(self, message: str) -> Optional[DecisionModel]:
        """
        规则优先判断：O(1) 关键词匹配，无 LLM 调用。

        Returns:
            DecisionModel if matched, None to fall through to LLM-assisted decision.
        """
        msg_lower = message.lower().strip()

        # time_tool：时间/日期查询
        for kw in _TIME_KEYWORDS:
            if kw in msg_lower:
                logger.debug("rule matched time_tool for message: %r", message[:60])
                return DecisionModel(
                    action=DecisionAction.TOOL_CALL,
                    tool_name="time_tool",
                    tool_args={},
                )

        # echo_tool：回显前缀命令
        for prefix in _ECHO_PREFIXES:
            if msg_lower.startswith(prefix):
                text_to_echo = message[len(prefix):]
                logger.debug("rule matched echo_tool, echo_text=%r", text_to_echo[:60])
                return DecisionModel(
                    action=DecisionAction.TOOL_CALL,
                    tool_name="echo_tool",
                    tool_args={"text": text_to_echo},
                )

        return None

    async def _llm_assisted_decision(self, message: str) -> DecisionModel:
        """
        LLM 辅助判断：使用 temperature=0.0 保证稳定 JSON 输出。

        兜底策略：任何异常（超时/解析失败/字段非法）均退回 direct_answer，
        保证 Agent 不因决策步骤崩溃而中断请求。
        """
        messages: List[Dict] = [
            {"role": "system", "content": _DECISION_SYSTEM_PROMPT},
            {"role": "user", "content": message},
        ]
        try:
            result: LLMResult = await self._llm_client.generate_with_messages(
                messages, temperature=0.0
            )
            json_text = _extract_json(result.answer)
            decision = DecisionModel.model_validate_json(json_text)
            logger.debug("llm decision: action=%s tool=%s", decision.action, decision.tool_name)
            return decision
        except Exception as exc:
            # JSON 解析失败、字段枚举不合法、LLM 超时等均兜底为 direct_answer
            logger.warning("llm_assisted_decision failed (%s), fallback to direct_answer", exc)
            return DecisionModel(action=DecisionAction.DIRECT_ANSWER)

    # ── tool 执行 ───────────────────────────────────────────────────────────────

    def _execute_tool(self, request_id: str, decision: DecisionModel) -> Tuple[str, str]:
        """
        执行 tool，返回 (tool_used, tool_result_str)。

        tool 失败时不抛异常，而是返回 ("", "") 并退回 direct_answer 语义，
        让调用方知道 tool 未成功但请求可继续。
        """
        if not decision.tool_name:
            return "", ""
        resp = self._tool_router.execute(
            ToolInvokeRequest(
                request_id=request_id,
                tool_name=decision.tool_name,
                tool_args=decision.tool_args or {},
            )
        )
        if resp.ok:
            logger.info("tool_used=%s result=%r", resp.tool_name, resp.output[:80])
            return resp.tool_name, resp.output
        # 工具失败不抛异常，降级 direct_answer，避免单个工具异常拖垮主链路
        logger.warning(
            "tool failed request_id=%s tool=%s code=%s detail=%s",
            resp.request_id,
            resp.tool_name,
            resp.error_code,
            resp.error_message,
        )
        return "", ""

    # ── 上下文组装 ──────────────────────────────────────────────────────────────

    def _build_final_messages(
        self,
        message: str,
        tool_used: str,
        tool_result: str,
    ) -> List[Dict]:
        """
        组装最终 LLM 请求的 messages。

        有 tool 结果时：以 user→assistant→user 三轮形式注入，
        assistant 标记 tool 调用，第二个 user 提供 tool 结果并要求回答原始问题。
        这种注入方式对 OpenAI-compatible API 兼容性最好（避免 function_call role）。
        """
        system = "You are Cyrus-GW agent. Answer concisely and accurately."
        if tool_used and tool_result:
            return [
                {"role": "system", "content": system},
                {"role": "user", "content": message},
                {"role": "assistant", "content": f"[I used {tool_used} to get information.]"},
                {
                    "role": "user",
                    "content": (
                        f"Tool '{tool_used}' returned: {tool_result}\n\n"
                        "Based on this result, answer my original question."
                    ),
                },
            ]
        return [
            {"role": "system", "content": system},
            {"role": "user", "content": message},
        ]

    # ── 核心编排（plan 阶段）────────────────────────────────────────────────────

    async def _plan(self, req: AgentChatRequest) -> Tuple[str, str, List[Dict]]:
        """
        决策 + tool 执行，返回 (tool_used, tool_result, final_messages)。

        职责分离：plan 不涉及 LLM 最终回答，只确定「用什么工具、结果是什么」，
        让 handle_chat 和 handle_chat_stream_with_tool_info 复用同一套逻辑。
        """
        # Step 1: 三级决策
        decision = self._rule_based_decision(req.message)
        if decision is None:
            decision = await self._llm_assisted_decision(req.message)

        # Step 2: tool 执行（失败时 tool_used="" 自动降级）
        tool_used, tool_result = "", ""
        if decision.action == DecisionAction.TOOL_CALL:
            # request_id 必须透传到 Tool 协议层，避免 Agent/Tool 日志无法对齐
            req_id = req.request_id or "req_tool_unknown"
            tool_used, tool_result = self._execute_tool(req_id, decision)

        # Step 3: 组装最终 messages
        final_messages = self._build_final_messages(req.message, tool_used, tool_result)
        return tool_used, tool_result, final_messages

    # ── 公开接口 ────────────────────────────────────────────────────────────────

    async def handle_chat(self, req: AgentChatRequest) -> AgentChatResponse:
        """非流式处理：完整走 plan → LLM → 返回结构化响应。"""
        request_id = req.request_id or f"req_{uuid.uuid4().hex[:16]}"

        tool_used, _, final_messages = await self._plan(req)
        llm_result: LLMResult = await self._llm_client.generate_with_messages(final_messages)

        return AgentChatResponse(
            request_id=request_id,
            answer=llm_result.answer,
            tool_used=tool_used,
            model=llm_result.model,
            retry_count=llm_result.retry_count,
        )

    async def handle_chat_stream_with_tool_info(
        self,
        req: AgentChatRequest,
    ) -> Tuple[str, AsyncIterator[str]]:
        """
        流式处理：先同步完成 plan（决策+tool），再返回 (tool_used, delta_stream)。

        设计意图：将 tool_used 提前暴露给 api.py，使 SSE 结束日志可正确记录 tool_used，
        而不是一律写空串。plan 本身不产生任何 SSE chunk，不影响客户端首包时延。
        """
        tool_used, _, final_messages = await self._plan(req)

        async def _stream() -> AsyncIterator[str]:
            async for delta in self._llm_client.generate_stream_with_messages(final_messages):
                yield delta

        return tool_used, _stream()

    # 向后兼容占位，仅供未来可能的旧调用路径使用
    async def handle_chat_stream(self, req: AgentChatRequest) -> AsyncIterator[str]:
        _, stream = await self.handle_chat_stream_with_tool_info(req)
        async for delta in stream:
            yield delta

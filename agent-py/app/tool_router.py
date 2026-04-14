"""
模块职责：tool 路由器，根据 tool_name 在白名单注册表中查找并派发执行。

关键分支原因：
- ToolNotFoundError (非 KeyError)：明确语义为"未注册工具"，调用方可区分
  配置问题（工具名拼写错）vs 执行问题（工具内部崩溃）
- ToolExecutionError：包装工具内部异常，让 AgentCore 捕获并降级而不是崩溃
- importlib 动态导入：工具模块在首次路由时加载，避免启动时全量导入

易踩坑点：
- tool_args 需与工具函数签名严格匹配：time_tool.run() 无参数，
  echo_tool.run(text) 需要 text 字段；LLM 决策漏填会触发 TypeError
- **tool_args 展开时若 LLM 多填了多余字段，也会触发 TypeError；
  工具函数建议用 **kwargs 接收额外字段（可选防御措施）
- 注册表为模块级常量，Agent 进程重启才生效；动态注册不在 MVP 范围内
"""

import importlib
import logging
from typing import Any, Dict, Tuple

from app.tools.contracts import ToolErrorCode, ToolInvokeRequest, ToolInvokeResponse

logger = logging.getLogger(__name__)


class ToolNotFoundError(RuntimeError):
    """工具名不在注册表中。"""


class ToolExecutionError(RuntimeError):
    """工具执行过程中抛出异常。"""


# 白名单注册表：tool_name -> (module_path, function_name)
# 扩展工具时只需在此添加一行，无需修改路由逻辑
_TOOL_REGISTRY: Dict[str, Tuple[str, str]] = {
    "time_tool": ("app.tools.time_tool", "run"),
    "echo_tool": ("app.tools.echo_tool", "run"),
}


class ToolRouter:
    """
    将 (tool_name, tool_args) 派发到对应工具模块并返回字符串结果。

    设计取舍：结果统一转 str，方便注入 LLM 上下文；工具内部返回其他类型
    （如 datetime）由 str() 兜底，不强制工具层做类型转换。
    """

    def execute(self, req: ToolInvokeRequest) -> ToolInvokeResponse:
        """
        按统一协议执行 Tool，返回结构化输出（不抛异常）。

        关键分支原因：
        - unknown tool -> TOOL_NOT_FOUND：可控失败，便于上游按错误码降级
        - bad args (非 dict/参数不匹配) -> TOOL_BAD_ARGS：提示调用方参数问题
        - runtime error -> TOOL_RUNTIME_ERROR：工具内部异常，不让进程崩溃

        易踩坑点：
        - req.request_id 必须原样回填到响应，确保日志链路可追踪
        - error_message 仅放文本，避免异常对象不可序列化影响 JSON 输出
        """
        entry = _TOOL_REGISTRY.get(req.tool_name)
        if entry is None:
            return ToolInvokeResponse(
                request_id=req.request_id,
                tool_name=req.tool_name,
                ok=False,
                error_code=ToolErrorCode.TOOL_NOT_FOUND,
                error_message=f"unknown tool: {req.tool_name!r}; available: {list(_TOOL_REGISTRY)}",
            )

        if not isinstance(req.tool_args, dict):
            return ToolInvokeResponse(
                request_id=req.request_id,
                tool_name=req.tool_name,
                ok=False,
                error_code=ToolErrorCode.TOOL_BAD_ARGS,
                error_message=f"tool_args must be dict, got {type(req.tool_args).__name__}",
            )

        module_path, func_name = entry
        try:
            mod = importlib.import_module(module_path)
            func = getattr(mod, func_name)
            result = func(**req.tool_args)
            return ToolInvokeResponse(
                request_id=req.request_id,
                tool_name=req.tool_name,
                ok=True,
                output=str(result),
                error_code=ToolErrorCode.NONE,
                error_message="",
            )
        except TypeError as exc:
            logger.warning("tool %r bad args: %s", req.tool_name, exc)
            return ToolInvokeResponse(
                request_id=req.request_id,
                tool_name=req.tool_name,
                ok=False,
                error_code=ToolErrorCode.TOOL_BAD_ARGS,
                error_message=str(exc),
            )
        except Exception as exc:
            logger.warning("tool %r raised: %s", req.tool_name, exc)
            return ToolInvokeResponse(
                request_id=req.request_id,
                tool_name=req.tool_name,
                ok=False,
                error_code=ToolErrorCode.TOOL_RUNTIME_ERROR,
                error_message=str(exc),
            )

    def route(self, tool_name: str, tool_args: Dict[str, Any]) -> str:
        """
        执行工具并返回字符串结果。

        Args:
            tool_name: 注册表中的工具名
            tool_args: 工具调用参数，由 ** 展开传入函数

        Returns:
            工具执行结果的字符串表示

        Raises:
            ToolNotFoundError: tool_name 不在注册表中
            ToolExecutionError: 工具函数执行期间抛出异常
        """
        # 兼容旧调用：复用 execute()，统一错误分支语义
        resp = self.execute(
            ToolInvokeRequest(
                request_id="req_tool_router_compat",
                tool_name=tool_name,
                tool_args=tool_args,
            )
        )
        if resp.ok:
            return resp.output
        if resp.error_code == ToolErrorCode.TOOL_NOT_FOUND:
            raise ToolNotFoundError(resp.error_message)
        raise ToolExecutionError(resp.error_message)

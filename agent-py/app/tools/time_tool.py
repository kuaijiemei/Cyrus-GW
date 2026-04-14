"""
模块职责：time_tool —— 返回当前 UTC 时间的 ISO 8601 字符串。

关键分支原因：
- 使用 timezone.utc 而非 datetime.utcnow()：后者不带时区信息，在对比/序列化时
  容易与本地时区混淆；带 tzinfo 的 datetime 序列化后含 "+00:00" 后缀，语义明确

易踩坑点：
- run() 无参数：tool_router 用 ** 展开 tool_args={}，若意外传入多余 key 会
  触发 TypeError；LLM 决策应确保 time_tool 的 tool_args 始终为 {}
- 返回类型为 str（isoformat），而非 datetime 对象；由 ToolRouter.route() 再 str() 包裹一次无副作用
"""

from datetime import datetime, timezone


def run() -> str:
    """返回当前 UTC 时间的 ISO 8601 字符串，例如 '2026-04-14T12:30:00+00:00'。"""
    return datetime.now(timezone.utc).isoformat()

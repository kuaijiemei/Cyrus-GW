"""
模块职责：echo_tool —— 原样回显输入文本，用于演示 tool pipeline 可通路。

关键分支原因：
- 参数名必须为 text：tool_router 用 ** 展开 tool_args，key 必须与函数参数名完全匹配
- 返回 str(text) 而非 text：防御 LLM 误传非字符串类型（如 int），避免下游注入上下文时崩溃

易踩坑点：
- LLM 决策应在 tool_args 中填入 {"text": "..."} 字段，若填成 {"content": "..."} 则 TypeError
- echo_tool 本身不做截断，若 LLM 传入超长文本会直接注入上下文；MVP 阶段不做长度限制
"""


def run(text: str) -> str:
    """原样回显 text，用于验证 tool 调用链路可通。"""
    return str(text)

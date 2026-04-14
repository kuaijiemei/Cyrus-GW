"""Agent 配置：敏感项仅来自环境变量，不入库。"""

from functools import lru_cache

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class AgentSettings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    llm_base_url: str = ""
    llm_api_key: str = ""
    llm_model: str = Field(default="gpt-4o-mini")
    llm_timeout_ms: int = Field(default=10_000)
    llm_retry_max: int = Field(default=1)
    agent_tool_enable_list: str = Field(default="time_tool,echo_tool")
    agent_memory_max_turns: int = Field(default=8)


@lru_cache
def get_settings() -> AgentSettings:
    return AgentSettings()

# Cyrus-GW 技术设计文档（定稿版）

## 1. 项目最终定义

### 项目名称

`Cyrus-GW：C++20/io_uring 驱动的高并发 AI Gateway 与 Agent Runtime`

### 一句话定位（必须背）

面向 LLM 场景构建高性能网关：基于 C++20 无栈协程与 io_uring 实现请求接入、异步调度、SSE 透传与限流控制；通过 Python Agent 完成任务决策与 Tool 调用闭环，并以同机基准测试验证并发模型性能差异。

### 本质（最重要）

本项目是：

- 高性能网关（C++） + AI 执行系统（Python）

不是：

- 纯 AI 项目
- 纯 WebServer

---

## 2. 系统架构（两服务架构）

```text
Client
   ↓
[C++ Gateway Service]   ← 高并发核心
   ↓ HTTP + JSON
[Python Agent Service]  ← AI逻辑核心
   ↓
LLM / Tools
```

说明：

- 对外入口只有 C++ Gateway 的 `/chat`。
- Gateway 不承载复杂 AI 逻辑，只做接入、调度、限流、转发、回包。
- Agent 负责任务理解、LLM 调用、tool 决策与结果组织。

---

## 3. 技术栈选择（最终版）

## 3.1 C++ Gateway（核心）

- C++20（`Coroutines`）
- 主实现：`Coroutines + io_uring`
- 对照实现（压测对比用）：`epoll + 线程池`（最小可运行即可）
- 非阻塞 I/O
- 多线程（I/O 线程 + 工作线程）

## 3.2 Python Agent Service

- FastAPI
- LLM API（OpenAI / 本地兼容接口）
- 基础 tool 执行框架

## 3.3 服务间通信（重要）

- **HTTP + JSON**
- 不使用 `gRPC`（当前版本明确禁止）

## 3.4 可选加分

- Redis（限流状态或 memory 持久化，非强制）
- Docker（部署与演示加分，非必须）

## 3.5 开发部署方式（已确定）

- IDE 方式：Cursor Remote SSH 直连 RHEL 虚拟机开发。
- 代码管理：在虚拟机仓库内执行 Git 提交并推送 GitHub。
- 运行策略：构建、联调、压测统一在虚拟机中完成。
- 当前虚拟机基线：
  - vCPU：4
  - RAM：15 GiB
  - Kernel：`5.14.0`
  - 根目录可用空间：18 GiB
  - 共享目录：`/mnt/hgfs`（用于传输，不作为主构建目录）
- 工程约束：
  - 主工程目录建议放在 Linux 本地文件系统（如 `~/Cyrus-GW`）。
  - `/mnt/hgfs` 仅用于素材同步，避免在共享目录进行高频编译。

---

## 4. 模块划分（精简可实现版）

## 4.1 C++ Gateway Service（王牌模块）

### 职责

- 提供 HTTP API：`/chat`
- 高并发请求处理
- 请求转发到 Python Agent
- 将 Agent 结果返回给客户端（支持 `SSE`）

### 你能讲的技术点

- Reactor vs Proactor
- 协程调度模型
- 非阻塞网络编程与高并发处理

## 4.2 调度与限流模块（工程能力）

### 功能

- 限流（Token Bucket）
- 超时控制
- 统一错误映射与重试边界

### 当前交付状态（v1.0.0）

- 单机内存限流计数
- Gateway 运行时直接走三种网络 I/O 模式之一：`blocking` / `epoll` / `io_uring`
- `scheduler/` 目录当前仍是占位实现，未接入独立请求队列
- Redis 仍为后续增强项，不作为当前版本依赖

## 4.3 Python Agent Service（AI 核心）

### 职责

- 接收 Gateway 下发任务
- 调用 LLM
- 判断是否调用 tool
- 返回结构化结果

### Agent 必备能力

1. 理解任务
2. 判断路径：
   - 直接回答
   - 调用工具
3. 组织最终输出

## 4.4 Tool 系统（亮点）

### MVP 工具示例

- `time_tool`：查时间
- `echo_tool`：回显参数（用于演示 tool pipeline）

### 面试关键问法对应

- “AI 怎么决定用工具？”  
  回答：由 Agent 的规则 + LLM 判断共同决定，输出结构化 action，再执行 tool。

---

## 5. 项目结构（实际仓库）

```text
Cyrus-GW/
  ├─ gateway-cpp/
  │   ├─ CMakeLists.txt
  │   ├─ configs/
  │   │   └─ gateway.yaml
  │   └─ src/
  │       ├─ main.cpp
  │       ├─ api/
  │       │   ├─ chat_handler.cpp
  │       │   └─ chat_handler.h
  │       ├─ common/
  │       │   ├─ config.h
  │       │   ├─ config_loader.cpp
  │       │   ├─ coroutine_compat.h
  │       │   ├─ errors.h
  │       │   ├─ json_util.h
  │       │   ├─ log_fields.h
  │       │   ├─ logger.cpp
  │       │   ├─ logger.h
  │       │   └─ models.h
  │       ├─ limiter/
  │       │   ├─ token_bucket.cpp
  │       │   └─ token_bucket.h
  │       ├─ net/
  │       │   ├─ epoll_server.cpp
  │       │   ├─ epoll_server.h
  │       │   ├─ event_loop.cpp
  │       │   ├─ http_common.h
  │       │   ├─ http_server.cpp
  │       │   ├─ http_server.h
  │       │   ├─ iouring_server.cpp
  │       │   ├─ iouring_server.h
  │       │   └─ uring_compat.h
  │       ├─ scheduler/
  │       │   ├─ task_queue.cpp
  │       │   └─ worker_pool.cpp
  │       ├─ upstream/
  │       │   ├─ agent_client.cpp
  │       │   └─ agent_client.h
  │   └─ scripts/
  │       ├─ verify_1_2.sh
  │       ├─ verify_1_4.sh
  │       ├─ verify_2_1.sh
  │       ├─ verify_2_2.sh
  │       ├─ verify_2_3.sh
  │       ├─ verify_2_4.sh
  │       └─ verify_2_5.sh
  ├─ agent-py/
  │   ├─ config.py
  │   ├─ requirements.txt
  │   ├─ app/
  │   │   ├─ __init__.py
  │   │   ├─ agent_core.py
  │   │   ├─ main.py
  │   │   ├─ api.py
  │   │   ├─ errors.py
  │   │   ├─ llm_client.py
  │   │   ├─ log_fields.py
  │   │   ├─ schemas.py
  │   │   ├─ tool_router.py
  │   │   └─ tools/
  │   │       ├─ __init__.py
  │   │       ├─ contracts.py
  │   │       ├─ time_tool.py
  │   │       └─ echo_tool.py
  │   └─ scripts/
  │       └─ verify_1_3.sh
  ├─ scripts/
  │   ├─ bench_week4_2.sh
  │   ├─ stream_ttft_bench.py
  │   ├─ verify_week1.sh
  │   ├─ verify_week2_1.sh
  │   ├─ verify_week2_2.sh
  │   ├─ verify_week2_3.sh
  │   ├─ verify_week2_4.sh
  │   ├─ verify_week2_5.sh
  │   ├─ verify_week3_1.sh
  │   ├─ verify_week3_2.sh
  │   ├─ verify_week3_3.sh
  │   ├─ verify_week3_5.sh
  │   ├─ verify_week4_1.sh
  │   ├─ verify_week4_2.sh
  │   ├─ verify_week4_4.sh
  │   └─ wrk_chat_non_stream.lua
  ├─ PRD.md
  ├─ RESEARCH.md
  ├─ TECH_DESIGN.md
  ├─ README.md
  ├─ AGENTS.md
  ├─ BENCHMARK_RESULTS.md
  ├─ DEMO_SCRIPT.md
  ├─ TIL.md
  └─ TODO.md
```

---

## 6. 数据模型

## 6.1 Client -> Gateway：`/chat` 请求

```json
{
  "message": "现在几点？",
  "stream": false,
  "session_id": "s1"
}
```

字段：

- `message`：用户输入（必填）
- `stream`：是否流式返回（可选）
- `session_id`：预留给未来 memory 扩展；`v1.0.0` 仅透传

## 6.2 Gateway -> Agent 请求模型

```json
{
  "request_id": "req_xxx",
  "message": "现在几点？",
  "stream": false,
  "session_id": "s1"
}
```

## 6.3 Agent 决策模型（内部）

```json
{
  "action": "tool_call",
  "tool_name": "time_tool",
  "tool_args": {}
}
```

`action` 取值：

- `direct_answer`
- `tool_call`

## 6.4 Agent -> Gateway 响应模型

非流式：

```json
{
  "request_id": "req_xxx",
  "answer": "现在是 21:30",
  "tool_used": "time_tool",
  "model": "gpt-4o-mini",
  "retry_count": 0
}
```

流式：

- 通过 `SSE` 向 Gateway 返回 chunk，Gateway 再透传给 Client。

---

## 7. 关键技术点

## 7.1 高并发 I/O 设计（C++）

- 事件驱动 + 非阻塞 I/O
- 请求处理协程化，降低回调复杂度
- 线程职责分离：网络线程与业务线程解耦

## 7.2 Reactor vs Proactor（可讲述）

- epoll 路径：偏 Reactor
- `io_uring` 路径：偏 Proactor
- 通过统一抽象接口屏蔽底层差异，便于切换

## 7.3 调度与排队

- 当前交付版没有单独请求队列；连接直接在所选 I/O 模式中处理
- `scheduler/task_queue.cpp` 与 `scheduler/worker_pool.cpp` 仍为占位文件，保留后续扩展点
- `queue_wait_ms` 尚未落地，不能作为当前版本的可观测指标承诺

## 7.4 Token Bucket 限流

- 限流维度：全局或按 IP（二选一）
- 超限行为：立即返回 `429`
- 配置项：桶容量、令牌填充速率

## 7.5 Gateway -> Agent 转发策略

- 短超时（如 3~10s）+ 轻量重试（最多 1 次）
- 失败返回标准错误码（`502/504`）
- 保留 `request_id` 贯穿链路

## 7.6 Agent 决策机制（灵魂）

- 规则优先：关键词命中直接走工具
- LLM 辅助：复杂请求由模型返回结构化决策
- tool 结果回填上下文，再组织最终回答

## 7.7 SSE 透传

- Gateway 不缓存全量结果，按 chunk 透传
- 控制首包超时与总超时
- 慢客户端触发背压，防止内存膨胀

## 7.8 并发模型对比压测（面试关键）

- 对比对象：
  - 主实现：`Coroutines + io_uring`
  - 对照实现：`epoll + 线程池`（仅最小链路，不要求完整功能覆盖）
- 压测范围：固定请求体，至少覆盖并发 `100/300/500`
- 指标采集：吞吐、平均延迟、`P95/P99`、错误率；流式场景记录 `TTFT`
- 产出：同机环境下的对比结果表（Markdown 即可）+ 结论说明（为何主线选择 `Coroutines + io_uring`）

---

## 8. 端到端流程

## 8.1 普通对话

1. Client 调用 Gateway `/chat`
2. Gateway 校验参数、限流、进入当前 I/O 模式处理路径
3. Gateway 转发到 Agent
4. Agent 直接调用 LLM 得到答案
5. Agent 返回结果给 Gateway
6. Gateway 回包给 Client

## 8.2 工具调用对话

1. Client 调用 `/chat`
2. Agent 判断需调用 tool
3. Agent 执行 `time_tool` 或 `echo_tool`
4. Agent 将 tool 结果与原问题一起交给 LLM
5. Agent 组织最终答案并返回 Gateway
6. Gateway 返回 Client

---

## 9. 非目标（必须记住）

当前版本明确不做：

- 分布式系统
- 多 Agent 协作
- `gRPC`
- 多租户
- cost 计费
- 复杂调度算法

可在面试中回答：后续可扩展，但当前版本刻意收敛，保证 4 周可落地。

---

## 10. 可观测性与验收

## 10.1 最小日志字段

- `request_id`
- `path`
- `latency_ms`
- `stream`
- `tool_used`
- `status_code`
- `retry_count`

条件字段：

- `ttft_ms`（仅流式成功路径）
- `error_layer`（错误路径）

预留未落地：

- `queue_wait_ms`
- `llm_call_latency_ms`

## 10.2 性能验收建议

- 并发 100/300/500 压测结果
- `SSE` 模式 TTFT 指标
- 限流触发正确返回 `429`
- 提供 `Coroutines + io_uring` vs `epoll + 线程池` 同环境对比结果与结论

## 10.3 开发问题沉淀验收（流程要求）

- 必须维护 `TIL.md`，用于记录模块开发中的真实问题与复盘。
- 每完成一个模块（如 `/chat`、SSE、限流、tool、压测）至少新增 1 条记录。
- 每条记录至少包含：
  - 现象
  - 根因
  - 解决方案
  - 防复发措施
  - 验证方式（正常路径 + 异常路径）

---

## 11. 结论

该技术设计的目标不是做“大而全”系统，而是做“可实现、可演示、可讲清”的双服务项目：  
**C++ Gateway 展示高并发工程能力，Python Agent 展示 AI 执行能力**，两者组合形成面试中最有辨识度的项目闭环。

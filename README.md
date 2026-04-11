# Cyrus-GW

`Cyrus-GW：C++20/io_uring 驱动的高并发 AI Gateway 与 Agent Runtime`

面向 LLM 场景构建高性能网关：基于 C++20 无栈协程与 `io_uring` 实现请求接入、异步调度、SSE 透传与限流控制；通过 Python Agent 完成任务决策与 Tool 调用闭环，并以同机基准测试验证并发模型性能差异。

---

## 1. 项目目标与边界

### 1.1 目标（MVP）

- 在 4 周内交付“可演示、可压测、可讲解”的双服务项目。
- 对外统一入口为 `Gateway /chat`。
- 支持非流式与流式（SSE）输出。
- 支持基础限流、超时控制、最多 1 次重试。
- 支持 Agent 决策与至少 1 条可复现 tool 调用路径。
- 输出并发模型对比压测结果（主实现 vs 对照实现）。

### 1.2 非目标（本期禁止）

- 分布式系统
- 多 Agent 协作
- `gRPC`
- 多租户
- cost 计费
- 复杂策略引擎

---

## 2. 系统架构

```text
Client
  |
  v
[C++ Gateway Service]  (高并发接入、调度、SSE 透传、限流)
  |
  | HTTP + JSON
  v
[Python Agent Service] (任务理解、LLM 调用、tool 决策)
  |
  v
LLM Provider / Tools
```

### 2.1 设计原则

- **职责分离**：Gateway 不做复杂 AI 逻辑，Agent 不做高并发网络接入。
- **链路可观测**：`request_id` 贯穿 Gateway -> Agent -> LLM。
- **最小可运行**：优先保证主链路完整，再做增强项。
- **可对比**：主实现 `Coroutines + io_uring`，对照实现 `epoll + 线程池`。

---

## 3. 目录建议结构

```text
Cyrus-GW/
  ├─ gateway-cpp/
  │   ├─ CMakeLists.txt
  │   ├─ configs/
  │   │   └─ gateway.yaml
  │   └─ src/
  │       ├─ main.cpp
  │       ├─ net/
  │       ├─ api/
  │       ├─ scheduler/
  │       ├─ limiter/
  │       ├─ upstream/
  │       └─ common/
  ├─ agent-py/
  │   ├─ requirements.txt
  │   ├─ app/
  │   │   ├─ main.py
  │   │   ├─ api.py
  │   │   ├─ schemas.py
  │   │   ├─ agent_core.py
  │   │   ├─ llm_client.py
  │   │   ├─ tool_router.py
  │   │   └─ tools/
  │   └─ config.py
  ├─ PRD.md
  ├─ TECH_DESIGN.md
  ├─ AGENTS.md
  ├─ README.md
  └─ TODO.md
```

---

## 4. 核心功能清单

### 4.1 `Gateway /chat`（P0）

- 接收 JSON 请求：
  - `message`（必填）
  - `stream`（可选，默认 `false`）
  - `session_id`（可选）
- 参数校验
- 限流判断（超限返回 `429`）
- 进入调度队列
- 转发到 Agent Service
- 回包给客户端（非流式或 SSE 流式）

### 4.2 Gateway -> Agent 转发（P0）

- 协议：HTTP + JSON
- 保留 `request_id`
- 超时控制（建议 3~10 秒）
- 最多 1 次重试（重点覆盖超时场景）
- 上游失败标准化映射（`502/504`）

### 4.3 Python Agent（P0）

- 接收 Gateway 任务
- 任务决策：
  - `direct_answer`
  - `tool_call`
- 调用 LLM
- 可调用最小 tool 集：
  - `time_tool`
  - `echo_tool`
- 返回结构化响应

### 4.4 SSE 透传（P0）

- 不等待全量结果，按 chunk 透传
- 控制首包超时与总超时
- 慢客户端背压防护（避免内存膨胀）

### 4.5 基础限流（P0）

- 方案：Token Bucket（推荐）
- 维度：全局或按 IP 二选一
- 配置项：
  - 桶容量
  - 填充速率
  - 可选突发窗口

### 4.6 简单 memory（P1，可选）

- 基于 `session_id` 缓存最近 N 轮消息
- 单进程内存存储，无持久化
- 达到上限按 FIFO 淘汰

---

## 5. API 合同（MVP）

### 5.1 Client -> Gateway: `POST /chat`

请求示例（非流式）：

```json
{
  "message": "现在几点？",
  "stream": false,
  "session_id": "s1"
}
```

请求示例（流式）：

```json
{
  "message": "请分三点介绍 io_uring",
  "stream": true,
  "session_id": "s2"
}
```

### 5.2 Gateway -> Agent 请求模型

```json
{
  "request_id": "req_xxx",
  "message": "现在几点？",
  "stream": false,
  "session_id": "s1"
}
```

### 5.3 Agent 决策模型（内部）

```json
{
  "action": "tool_call",
  "tool_name": "time_tool",
  "tool_args": {}
}
```

`action` 枚举：

- `direct_answer`
- `tool_call`

### 5.4 Agent -> Gateway 响应模型（非流式）

```json
{
  "request_id": "req_xxx",
  "answer": "现在是 21:30",
  "tool_used": "time_tool",
  "model": "gpt-4o-mini"
}
```

### 5.5 SSE 输出约定（建议）

- `event: delta`：普通 chunk
- `event: done`：结束
- `event: error`：错误

---

## 6. 开发环境与运行方式

### 6.1 平台说明

- 本地平台：Windows（无 WSL）
- 推荐主开发方式：Cursor Remote SSH 连接 RHEL 虚拟机
- 原因：`io_uring` 依赖 Linux 内核能力，压测结果更可信

### 6.2 Linux VM 基线（已确认）

- vCPU：4
- RAM：15 GiB
- Kernel：`5.14.0`
- 根目录可用：18 GiB（需定期清理构建产物）
- 共享目录：`/mnt/hgfs`（仅用于互传，不作为主构建目录）

### 6.3 远程环境初始化

```bash
sudo dnf makecache
sudo dnf install -y gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config git curl python3 python3-pip
```

可选依赖：

- `liburing` + `liburing-devel`
- `wrk` 或 `hey`

---

## 7. 本地最小跑通（Linux VM）

### 7.1 启动 Agent Service

```bash
cd agent-py
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

要让 Gateway 的 `POST /chat` 经 Agent 调到真实 LLM，必须在**启动 uvicorn 之前**在同一终端里导出环境变量（密钥勿写入仓库）。**先 `export`，再运行 `uvicorn`一行**；若 uvicorn 已在运行，先 `Ctrl+C` 停掉再执行下面整段，否则进程里仍是没有 LLM 配置的旧环境，会一直返回 **503**（`llm_not_configured`），网关侧则常见 **502**。

```bash
export LLM_BASE_URL="https://api.openai.com/v1"
export LLM_API_KEY="这里填真实密钥，不要用占位符"
export LLM_MODEL="gpt-4o-mini"
export LLM_TIMEOUT_MS="10000"
python3 -m uvicorn app.main:app --host 0.0.0.0 --port 8001
```

说明：`Agent` 在进程启动时读取环境变量；在 uvicorn **运行过程中**再输入 `export` 不会生效。无真实上游时可用仓库根目录 `bash scripts/verify_week1.sh`（自带 Mock LLM，无需 API Key）。

### 7.2 构建并启动 Gateway

```bash
cd gateway-cpp
cmake -S . -B build -G Ninja
cmake --build build -j
./build/cyrus-gateway
```

#### RHEL：`dnf install cmake` 报「未找到匹配项」时

先用下面两条自检：

```bash
sudo dnf repolist
sudo dnf search cmake
```

若 **`repolist` 里只有 `mysql-*` 等少数仓库**，且终端提示 **系统尚未在权利服务器中注册**，则 **BaseOS / AppStream 未随订阅启用**，任何 `dnf install cmake` / `dnf search cmake` 都会失败（与包名无关）。

**处理方式（二选一或组合）：**

1. **启用 Red Hat 订阅**（由你或管理员完成）：`subscription-manager register` / `attach`（或公司 Satellite、`rhc connect` 等），直到 `dnf repolist` 中出现 `baseos`、`appstream` 一类仓库后，再执行：

   ```bash
   sudo dnf install -y cmake ninja-build
   ```

2. **在订阅修好之前**：不必装 cmake，直接用下面 **§7.2** 中的 **`g++` 一条命令** 即可编出 `build/cyrus-gateway`（与本仓库 `CMakeLists.txt` 源列表一致）。

若环境未安装 `cmake` / `ninja`，可在 `gateway-cpp` 目录下用 `g++` 直接链接（与仓库内 `CMakeLists.txt` 源文件列表一致）：

```bash
cd gateway-cpp
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Wpedantic -O2 \
  src/main.cpp src/common/config_loader.cpp src/common/logger.cpp \
  src/net/http_server.cpp src/net/event_loop.cpp \
  src/api/chat_handler.cpp src/scheduler/task_queue.cpp src/scheduler/worker_pool.cpp \
  src/limiter/token_bucket.cpp src/upstream/agent_client.cpp \
  -Isrc -o build/cyrus-gateway
./build/cyrus-gateway
```

说明：建议在 `gateway-cpp` 目录启动网关，以便默认读取 `configs/gateway.yaml`；若从 `build/` 启动，可传参 `../configs/gateway.yaml` 或使用可执行文件旁路径解析（见 `TIL.md`）。

### 7.3 基本验证

`-w '\n'` 会在响应体末尾多输出一个换行，避免 JSON 与下一行 shell 提示符 `]` 粘在一起；若要把 JSON 换行缩进，可接 `python3 -m json.tool`（已装 `jq` 时可用 `jq .`）。

```bash
# 可选：美化大模型返回的 JSON
curl -sS -w '\n' http://127.0.0.1:8080/health

curl -sS -w '\n' -X POST http://127.0.0.1:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"message":"hello","stream":false}'

#可选：不美化大模型返回的 JSON
curl -sS -X POST http://127.0.0.1:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"message":"hello","stream":false}' | python3 -m json.tool
```

### 7.4 Week 1 里程碑验证（TODO 1.5，可视化）

仓库根目录一键脚本（自动拉起 Mock LLM、Agent、Gateway，打印成功 / 网关错误 / 上游错误三段输出）：

前置：`agent-py` 已创建 `.venv` 并 `pip install -r requirements.txt`（脚本会检查 `.venv/bin/python`）；若尚无 `gateway-cpp/build/cyrus-gateway`，脚本会自动用 `g++` 编译一份。

```bash
bash scripts/verify_week1.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_1_4.sh
```

**最小日志样例（Gateway 进程 stderr，JSON 行）**

成功（`POST /chat` 经转发返回 200）：

```json
{"request_id":"req-e2e-1","latency_ms":69,"status_code":200,"tool_used":"","path":"/chat","stream":false}
```

网关侧校验失败（`400`，参数非法）：

```json
{"request_id":"req_xxxxxxxxxxxxxxxx","latency_ms":0,"status_code":400,"tool_used":"","path":"/chat","stream":false}
```

上游 Agent 不可用（`502`）：

```json
{"request_id":"req-upstream-down","latency_ms":0,"status_code":502,"tool_used":"","path":"/chat","stream":false}
```

（`request_id` 与 `latency_ms` 以你本机运行为准；字段名固定。）

Week 1 各子模块复盘见根目录 `TIL.md`（1.1–1.4 已各至少一条）。

---

## 8. 配置建议

### 8.1 Gateway 配置（示意）

建议在 `gateway-cpp/configs/gateway.yaml` 中包含：

- `listen_host`
- `listen_port`
- `agent_base_url`
- `agent_timeout_ms`
- `agent_retry_max`
- `rate_limit.capacity`
- `rate_limit.refill_per_sec`
- `queue.max_size`
- `sse.first_chunk_timeout_ms`
- `sse.total_timeout_ms`

### 8.2 Agent 配置（示意）

建议在 `agent-py/config.py` 读取：

- `LLM_BASE_URL`
- `LLM_API_KEY`
- `LLM_MODEL`
- `LLM_TIMEOUT_MS`
- `AGENT_TOOL_ENABLE_LIST`
- `AGENT_MEMORY_MAX_TURNS`

### 8.3 环境变量示例

```bash
export LLM_BASE_URL="https://api.openai.com/v1"
export LLM_API_KEY="YOUR_API_KEY"
export LLM_MODEL="gpt-4o-mini"
export LLM_TIMEOUT_MS="10000"
```

---

## 9. 日志与可观测性（最低要求）

每条请求日志至少包含：

- `request_id`
- `path`
- `latency_ms`
- `queue_wait_ms`
- `llm_call_latency_ms`
- `stream`
- `tool_used`
- `status_code`

建议增强字段：

- `retry_count`
- `client_ip`
- `error_code`
- `error_layer`（gateway / agent / llm）

---

## 10. 测试与验收

### 10.1 功能测试（必须）

- `/chat` 非流式成功
- `/chat` 流式 SSE 成功
- 至少 1 条 tool 调用路径可复现
- 限流触发返回 `429`
- LLM 超时触发最多 1 次重试

### 10.2 压测（必须）

对比对象：

1. 主实现：`Coroutines + io_uring`
2. 对照实现：`epoll + 线程池`

至少覆盖并发档位：

- 100
- 300
- 500

至少输出指标：

- 吞吐（RPS/QPS）
- 平均延迟
- `P95/P99`
- 错误率
- 流式场景 `TTFT`

### 10.3 回归建议

- 修改网关转发后：
  - 正常
  - 上游超时
  - 上游错误
  - 限流边界
- 修改 Agent 决策后：
  - 直接回答路径
  - tool 路径
  - 异常路径
- 每完成一个模块后：
  - 将开发中遇到的问题沉淀到 `TIL.md`
  - 至少记录 1 条“现象 + 根因 + 解决方案 + 防复发 + 验证方式”

---

## 11. 4 周里程碑（建议）

### Week 1：主链路打通

- 项目骨架初始化
- `/chat` 非流式闭环
- 日志与错误码基础规范

### Week 2：流式与稳态能力

- SSE 打通
- 基础限流
- 超时与 1 次重试策略

### Week 3：Agent 与 Tool

- Agent 决策机制
- `time_tool` 与 `echo_tool`
- 可选 memory（若进度允许）

### Week 4：压测与讲解材料

- 并发模型对比压测
- 数据整理与结论
- Demo 脚本与面试讲述打磨

---

## 12. Definition of Done（DoD）

以下条件全部满足才算完成：

1. 可在 Linux VM 编译运行
2. 功能自测通过（每模块至少 1 条正常 + 1 条异常）
3. 日志字段达到最低规范
4. 若架构有变更，已同步更新 `PRD.md` 与 `TECH_DESIGN.md`
5. 未引入 PRD 禁止项
6. 每个已完成模块至少 1 条开发问题复盘已写入 `TIL.md`

---

## 13. 常见问题（FAQ）

### Q1：为什么不直接把 AI 逻辑写进 C++ Gateway？

因为本项目强调“高并发工程能力 + AI 执行能力”的分层组合。Gateway 只做接入与调度，Agent 负责 AI 逻辑，职责清晰、可扩展、可讲解。

### Q2：为什么当前版本不做 gRPC？

MVP 阶段明确收敛范围，`HTTP + JSON` 足够支撑演示与压测，避免引入额外复杂度。

### Q3：为什么要做并发模型对比？

这是面试亮点。可量化展示 `Coroutines + io_uring` 的收益与工程取舍，而不是“只写了功能”。

### Q4：Windows 上能开发吗？

可编辑代码，但不建议作为主运行环境。`io_uring` 与压测应统一在 Linux VM 进行。

---

## 14. 推荐补充文档

为提升“可讲解性”，建议后续补齐：

- `INTERFACE_CONTRACT.md`：Gateway 与 Agent 的 HTTP 契约
- `BENCHMARK_PLAN.md`：压测命令、结果模板、结论模板
- `DEMO_SCRIPT.md`：5 分钟面试演示脚本
- `TIL.md`：模块开发问题与经验沉淀（每完成一个模块后更新）

---

## 15. 文档优先级（冲突处理）

当文档内容冲突时，按以下优先级执行：

1. `PRD.md`
2. `TECH_DESIGN.md`
3. `RESEARCH.md`
4. `AGENTS.md`

本 `README.md` 作为落地执行说明，不覆盖以上文档的决策优先级。

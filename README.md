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
  │       ├─ scheduler/          # TaskQueue + WorkerPool（调度抽象层）
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

### 7.5 Week 2 SSE 端到端验证（TODO 2.1，可视化）

仓库根目录一键脚本（自动拉起 Mock LLM、Agent、Gateway，依次打印：正常流式 / 首包超时 / 总超时中断 / 非流式回归）：

```bash
bash scripts/verify_week2_1.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_2_1.sh
```

脚本输出说明：

- `2.1-1`：期望看到 `event: delta`（多次）+ `event: done`
- `2.1-2`：期望返回 `HTTP 504`，`detail` 为 `first_chunk_timeout`
- `2.1-3`：期望看到首段 `delta` 后出现 `event: error`（`stream_total_timeout`）
- `2.1-4`：期望返回标准非流式 JSON（用于回归验证）

脚本会在 `/tmp/gateway_week2_1_*.log` 输出网关日志，包含 `ttft_ms`。

### 7.6 Week 2 限流验证（TODO 2.2，可视化）

仓库根目录一键脚本（自动拉起 Mock LLM、Agent、Gateway，依次打印：严格限流触发 429 / 调整参数后通过率提升 / 限流命中日志）：

```bash
bash scripts/verify_week2_2.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_2_2.sh
```

脚本输出说明：

- `2.2-1`：严格参数（`capacity=2, refill_per_sec=0`）下稳定触发 `429`
- `2.2-2`：宽松参数（`capacity=10, refill_per_sec=100`）下同批请求 `429` 显著减少
- `2.2-3`：网关日志可见 `status_code=429` 且 `tool_used=rate_limited`

### 7.7 Week 2 超时与重试验证（TODO 2.3，可视化）

仓库根目录一键脚本（自动拉起 mock Agent / mock LLM / Agent / Gateway，依次打印：Gateway 超时重试成功、Gateway 超时重试失败、LLM 超时重试成功、LLM 超时重试失败）：

```bash
bash scripts/verify_week2_3.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_2_3.sh
```

脚本输出说明：

- `2.3-1`：Gateway -> Agent 首次超时，重试 1 次成功（最终 200）
- `2.3-2`：Gateway -> Agent 连续超时，最多重试 1 次后返回 504
- `2.3-3`：Agent -> LLM 首次超时，重试 1 次成功（最终 200）
- `2.3-4`：Agent -> LLM 连续超时，最多重试 1 次后返回 504

日志应可观察 `retry_count`，并可通过 mock stats 看到调用次数为 2（无无限重试）。

### 7.8 Week 2 错误处理与状态码统一验证（TODO 2.4，可视化）

仓库根目录一键脚本（自动拉起 mock Agent / mock LLM / Agent / Gateway，依次打印：400、429、502、504 典型错误场景，并输出网关/Agent 错误层级日志）：

```bash
bash scripts/verify_week2_4.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_2_4.sh
```

脚本输出说明：

- `2.4-1`：`400`（非法 JSON），错误体含 `request_id` 与 `error_layer=gateway`
- `2.4-2`：`429`（限流），错误体含 `request_id` 与 `error_layer=gateway`
- `2.4-3`：`502`（Agent 不可用），错误体含 `request_id` 与 `error_layer=agent`
- `2.4-4`：`504`（上游超时），错误体含 `request_id` 与 `error_layer=agent`
- `2.4-5`：日志可见失败层级字段：Gateway 侧 `gateway/agent`，Agent 侧 `llm`

### 7.9 Week 2 里程碑总验证（TODO 2.5）

仓库根目录一键脚本（串行执行 2.1~2.4，完整演示：SSE、限流、超时重试、错误统一）：

```bash
bash scripts/verify_week2_5.sh
```

等价于：

```bash
bash gateway-cpp/scripts/verify_2_5.sh
```

该脚本适合里程碑汇报或面试演示，输出末尾会打印 `done: Week 2 milestone verify finished`。

### 7.10 Week 4.2 压测脚本与命令固化（TODO 4.2）

仓库根目录提供两类脚本：

- `scripts/bench_week4_2.sh`：固化并执行压测命令（非流式 + 流式 TTFT）
- `scripts/verify_week4_2.sh`：4.2 一键验收（中文彩色 PASS/FAIL/SKIP）

本轮排障后的关键行为：

- `scripts/bench_week4_2.sh` 在实际执行压测前，会先检查 `Gateway(:8080)` 与 `Agent(:8001)` 是否可用
- 若未运行，脚本会自动拉起 `mock LLM(:19943) + Agent(:8001) + Gateway(:8080)`，避免 TTFT 因上游未启动而出现整批 `502` 或 `timed out`
- 若服务已在运行，则直接复用，不重复起进程

先看固定命令（不执行）：

```bash
bash scripts/bench_week4_2.sh --mode=epoll --print-only
```

默认固化参数（脚本内置）：

- 并发档位：`100,300,500`
- 预热：`10s`
- 采样：`30s`
- 非流式工具：优先 `wrk`，缺失时降级 `hey`
- 流式：`python3 scripts/stream_ttft_bench.py` 记录 `TTFT avg/p50/p95/p99`

执行完整压测（会实际发请求）：

```bash
# epoll 对照实现
bash scripts/bench_week4_2.sh --mode=epoll

# io_uring 主实现（需内核启用 io_uring）
bash scripts/bench_week4_2.sh --mode=iouring
```

仅执行某一类：

```bash
# 仅非流式
bash scripts/bench_week4_2.sh --mode=epoll --run=non-stream

# 仅流式 TTFT
bash scripts/bench_week4_2.sh --mode=epoll --run=stream
```

4.2 一键验收：

```bash
bash scripts/verify_week4_2.sh
```

脚本覆盖：

- 命令固化项是否包含 `100/300/500` + 预热 + 采样时长
- 流式 TTFT 命令是否可执行（正常路径）
- 非法参数是否被拒绝（异常路径）

说明：仓库中已有 `scripts/verify_week3_2.sh`（Week 3 Tool 框架验收），Week 4.2 使用 `scripts/verify_week4_2.sh`，避免覆盖旧里程碑脚本。

### 7.11 本机对比测试步骤（epoll vs io_uring）

下面这组步骤适合你在当前 Linux VM 上直接做“小规模先确认脚本行为，再跑正式档位”的对比测试。

#### Step 1：确认环境

先确认 `agent-py/.venv` 已准备好，且 `gateway-cpp/build/cyrus-gateway` 已编译完成。

可选确认：

```bash
ls agent-py/.venv/bin/python
ls gateway-cpp/build/cyrus-gateway
```

若要跑 `io_uring` 对比，还需确认内核已启用：

```bash
sysctl kernel.io_uring_disabled
```

期望输出为 `kernel.io_uring_disabled = 0`；若不是，可先执行：

```bash
sudo sysctl -w kernel.io_uring_disabled=0
```

#### Step 2：从空端口状态开始

建议每次对比前都先确认 `8080/8001` 没有残留进程，让脚本完整走一次“自动拉起”路径：

```bash
lsof -i :8080
lsof -i :8001
```

若无输出，说明当前端口空闲。

#### Step 3：先跑小规模 smoke test

先用小并发验证脚本和链路是否稳定：

```bash
# epoll
bash scripts/bench_week4_2.sh --mode=epoll --run=stream --levels=2 --duration-sec=5 --warmup-sec=3

# io_uring
bash scripts/bench_week4_2.sh --mode=iouring --run=stream --levels=2 --duration-sec=5 --warmup-sec=3
```

期望关注点：

- 是否打印 `自动拉起 mock LLM + Agent + Gateway`
- TTFT 结果是否为 `成功/失败 : N/0`
- 是否输出 `TTFT avg/p50/p95/p99`

本轮已验证的一个样例结果是：

- `epoll` + `--levels=2` + `--run=stream` 下，TTFT 采样为 `4/0` 成功，`TTFT avg(ms) = 44.64`

#### Step 4：再跑正式对比

小规模确认通过后，再跑正式档位：

```bash
# epoll 对照实现
bash scripts/bench_week4_2.sh --mode=epoll

# io_uring 主实现
bash scripts/bench_week4_2.sh --mode=iouring
```

如果只想分别看吞吐和流式 TTFT，也可以拆开跑：

```bash
# 非流式吞吐/延迟
bash scripts/bench_week4_2.sh --mode=epoll --run=non-stream
bash scripts/bench_week4_2.sh --mode=iouring --run=non-stream

# 流式 TTFT
bash scripts/bench_week4_2.sh --mode=epoll --run=stream
bash scripts/bench_week4_2.sh --mode=iouring --run=stream
```

#### Step 5：记录对比结果

建议按下面四类指标整理 `epoll` 与 `io_uring` 两组输出：

- 非流式：`Requests/sec`
- 非流式：`Latency` 与 `P95/P99`
- 非流式：`Non-2xx or 3xx responses`
- 流式：`TTFT avg/p50/p95/p99`

这样后续进入 TODO 4.3 时，可以直接把同一台机器、同一脚本、同一档位下的结果整理成对比表。

### 7.12 压测结果速览（TODO 4.3）

完整数据与分析见 `BENCHMARK_RESULTS.md`。以下是关键对比结论（开发者本机亲测数据）：

**非流式吞吐（wrk, 15s 正式采样）**

| 并发 | epoll RPS | iouring RPS | 差异 |
|---|---|---|---|
| 100 | 37.22 | **37.99** | +2.1% |
| 300 | 24.39 | **25.04** | +2.7% |
| 500 | 6.03 | **13.00** | **+115.6%** |

**非流式延迟（C=100, P50/P99）**

| 指标 | epoll | iouring |
|---|---|---|
| P50 | 2.27 s | **2.24 s** |
| P99 | 4.38 s | **4.34 s** |

**流式 TTFT（正式采样，全档位 100% 成功）**

| 并发 | epoll P50 | iouring P50 | epoll P99 | iouring P99 |
|---|---|---|---|---|
| 100 | **2,163 ms** | 2,310 ms | **2,274 ms** | 2,744 ms |
| 300 | 6,763 ms | **6,734 ms** | 6,953 ms | **6,904 ms** |
| 500 | **11,279 ms** | 11,504 ms | **11,520 ms** | 11,775 ms |

核心结论：io_uring 在非流式极限并发（C=500）下吞吐是 epoll 的 **2.15 倍**，P50/P99 全档位优于 epoll。流式 TTFT 两者接近，因 Agent 单 worker 为主要瓶颈。详见 `BENCHMARK_RESULTS.md §5`。

### 7.14 Scheduler 调度层抽象（src/scheduler）

原先 epoll 路径的线程池队列内联在 `epoll_server.cpp`，io_uring 路径由 CQE 协程恢复隐式调度。
现已将调度能力抽象为独立组件：

| 文件 | 职责 |
|---|---|
| `scheduler/task_queue.h/.cpp` | 线程安全 fd 队列：max_size 容量限制、入队时间戳 + queue_wait_ms 统计、超时自动取消 |
| `scheduler/worker_pool.h/.cpp` | 固定 worker 线程池，从 TaskQueue 消费 fd 并执行 handler |

**epoll 路径**：使用 `TaskQueue` + `WorkerPool`（经典线程池调度），队列满时返回 503。

**io_uring 路径**：调度由 CQE 驱动协程恢复（Proactor 模型），accept 完成后记录时间戳计算 `queue_wait_ms`。

两条路径的日志均输出 `queue_wait_ms` 字段，可在 `gateway.yaml` 中配置：

```yaml
queue:
  max_size: 2000      # TaskQueue 容量上限（epoll 路径）
  timeout_ms: 0       # 入队超时（0=不限制）
```

### 7.13 演示与讲解材料（TODO 4.4）

完成 Week 4.4 后，项目新增以下演示材料：

| 文件 | 内容 |
|---|---|
| `DEMO_SCRIPT.md` | 架构图（双服务）、时序图（普通/Tool/流式 三种路径）、5 分钟演示流程（7 步口播 + curl 命令）、常见追问答辩点（15+ 问答）、TIL Top 5 问题提炼 |
| `scripts/verify_week4_4.sh` | 一键验证 §4.4 全部交付物（26 项检查，含正常路径 + 禁止项异常路径） |

**使用方式**：

```bash
# 验证 §4.4 全部交付物
bash scripts/verify_week4_4.sh

# 面试前快速过一遍演示流程
cat DEMO_SCRIPT.md
```

**演示时间分配建议**（5 分钟）：

1. 开场介绍（30s）→ 2. 非流式对话（60s）→ 3. 流式 SSE（60s）→ 4. Tool 调用（60s）→ 5. 限流（30s）→ 6. 压测数据（60s）→ 7. 总结（30s）

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

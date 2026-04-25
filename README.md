# Cyrus-GW

一个面向 LLM 流式接入场景的 C++20 AI Gateway：基于 io_uring + 协程实现异步 I/O 调度，并提供 epoll Reactor 对照基线、SSE 透传、Token Bucket 限流、request_id 链路日志与 wrk 压测分析。

---

## 1. 项目目标与边界

### 1.1 目标

- 4 周内完成可演示、可压测、可讲解的 Gateway / Agent 双服务 MVP
- 对外统一入口为 Gateway `/chat`
- 支持非流式与 SSE 流式输出
- 支持限流、超时、最多 1 次重试
- 支持 Agent 决策与至少 1 条可复现 Tool 调用路径
- 输出 `epoll` vs `io_uring` 并发模型对比压测结果

### 1.2 非目标

- 分布式系统
- 多 Agent 协作
- `gRPC`
- 多租户
- cost 计费
- 复杂策略引擎

### 1.3 v1.0.0 交付状态

- 已交付 `/chat` 非流式与流式（SSE）主链路
- 已交付 Gateway 限流、超时、最多 1 次重试与统一错误映射
- 已交付 Agent 规则优先 + LLM 结构化决策，以及 `time_tool` / `echo_tool` 最小工具集
- 已交付 `blocking` / `epoll` / `io_uring` 三种网络 I/O 模式与同机压测材料

`session_id` 已进入请求契约，当前版本仅透传预留；simple memory、Redis 与更细粒度调度指标未纳入 `v1.0.0`。

## 2. 系统架构

```text
Client
  |
  v
[C++ Gateway Service]  接入 / 限流 / 转发 / SSE 透传
  |
  | HTTP + JSON
  v
[Python Agent Service] 决策 / LLM 调用 / Tool 路由
  |
  v
LLM Provider / Tools
```

### 2.1 设计原则

- **职责分离**：Gateway 负责接入、限流、转发与 SSE 透传，Agent 负责决策、LLM 调用和 Tool 路由
- **链路可观测**：`request_id` 贯穿 Gateway -> Agent，并在 Agent 侧关联 LLM 调用日志，便于定位错误层级和性能瓶颈
- **最小可运行**：优先交付可跑通、可演示的主链路，再逐步补齐 SSE、限流、重试与压测材料
- **可对比压测**：保留 `epoll` 对照实现，与 `io_uring` 在同机、同脚本、同档位下做可复现实验

## 3. 目录结构

```text
Cyrus-GW/
├─ gateway-cpp/          # C++ Gateway：接入、限流、转发、SSE、io_uring/epoll
├─ agent-py/             # Python Agent：决策、LLM 调用、Tool Router
├─ scripts/              # 验证脚本与压测脚本
├─ BENCHMARK_RESULTS.md  # 压测数据与分析
├─ TECH_DESIGN.md        # 技术设计与接口细节
├─ DEMO_SCRIPT.md        # 演示流程与答辩材料
└─ TIL.md                # 开发问题复盘
```

## 4. 核心能力

- **`/chat` 统一接入**：Gateway 对外提供统一入口，接收 `message`、`stream`、`session_id`；其中 `session_id` 当前版本仅透传预留
- **Gateway -> Agent 转发**：协议使用 `HTTP + JSON`，保留并透传 `request_id`，处理上游超时、连接失败与状态码映射
- **SSE 透传**：支持首包超时、总超时，以及 `event: delta`、`event: done`、`event: error` 三类事件
- **稳定性机制**：Gateway 侧使用 Token Bucket 做基础限流；上游调用最多重试 1 次；上游失败统一映射为 `502/504`
- **Agent 闭环**：Agent 采用“规则优先 + LLM 辅助决策”的最小闭环，当前支持 `time_tool` 与 `echo_tool`
- **可观测性**：统一记录 `request_id`、`latency_ms`、`status_code`、`retry_count`、`error_layer`；流式成功路径额外记录 `ttft_ms`

最小请求示例：

```json
{
  "message": "现在几点？",
  "stream": false,
  "session_id": "s1"
}
```

最小响应示例：

```json
{
  "request_id": "req_xxx",
  "answer": "现在是 21:30",
  "tool_used": "time_tool",
  "model": "gpt-4o-mini",
  "retry_count": 0
}
```

详细接口模型、错误响应与配置说明见 [TECH_DESIGN.md](TECH_DESIGN.md)。

## 5. 关键挑战与解决方案

| 挑战 | 解决方案 | 验证结果 |
|---|---|---|
| HTTP 请求体可能因半包导致读取不完整 | 按 header 边界与 `Content-Length` 循环读取完整 body | 修复 body 截断与后续请求错位问题 |
| SSE 透传时 chunked 长度行可能泄漏到客户端 | Gateway 侧实现 `ChunkedDecoder` | 下游只接收标准 SSE 事件 |
| 首包后失败无法再修改 HTTP 状态码 | 首包前返回 `502/504`，首包后通过 `event:error` 表达错误 | 流式错误语义更稳定 |
| 完整链路吞吐受 Agent / LLM 限制 | 对比 Gateway 纯错误路径、Agent 单 worker、完整链路 | 定位瓶颈主要在 Agent / LLM 侧 |
| `epoll` 与 `io_uring` 差异难靠理论判断 | 同机 `wrk` 对照 `C=100/300/500` | `C=500` 下 `io_uring` 完成请求数约为 `epoll` 2.15 倍 |

## 6. 压测结果速览

### 6.1 非流式吞吐

| 并发 | epoll RPS | io_uring RPS | 差异 |
|---|---|---|---|
| 100 | 37.22 | 37.99 | +2.1% |
| 300 | 24.39 | 25.04 | +2.7% |
| 500 | 6.03 | 13.00 | +115.6% |

### 6.2 流式 TTFT

| 并发 | epoll P50 | io_uring P50 | epoll P99 | io_uring P99 |
|---|---|---|---|---|
| 100 | 2163 ms | 2310 ms | 2274 ms | 2744 ms |
| 300 | 6763 ms | 6734 ms | 6953 ms | 6904 ms |
| 500 | 11279 ms | 11504 ms | 11520 ms | 11775 ms |

非流式场景下，`io_uring` 在 `C=500` 高并发档位吞吐优势明显，完成请求数约为 `epoll` 的 `2.15` 倍。流式 TTFT 两者整体接近，主要瓶颈在 Agent 单 worker / LLM 调用侧，而不是 Gateway 接入层。完整数据、命令与分析见 [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md)。

## 7. 项目交付说明

本项目在 4 周内完成 Gateway / Agent 双服务 MVP 交付。开发过程中使用 Cursor / Claude 辅助方案校验、接口文档整理与异常路径复盘；本人重点负责需求拆解、架构取舍、核心链路联调、功能验收、压测对照与瓶颈分析。

## 8. 快速开始

以下命令默认在 Linux 环境执行，已安装 CMake、C++20 编译器、Python 3 与 pip；io_uring 对照压测建议在 Linux VM 上运行。

### 8.1 启动 Agent

```bash
cd agent-py
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 -m uvicorn app.main:app --host 0.0.0.0 --port 8001
```

### 8.2 启动 Gateway

```bash
cd gateway-cpp
cmake -S . -B build -G Ninja
cmake --build build -j
./build/cyrus-gateway
```

### 8.3 最小 curl 验证

```bash
curl -sS -w '\n' -X POST http://127.0.0.1:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"message":"hello","stream":false}'
```

环境排障见 [TIL.md](TIL.md)，压测命令与原始数据见 [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md)，演示流程见 [DEMO_SCRIPT.md](DEMO_SCRIPT.md)。

## 9. 演示与验证

- [DEMO_SCRIPT.md](DEMO_SCRIPT.md) 提供 5 分钟演示流程、关键路径说明和常见追问
- `scripts/verify_week*.sh` 用于回归验证
- `scripts/bench_week4_2.sh` 和 `scripts/stream_ttft_bench.py` 用于压测与 TTFT 采样

## 10. FAQ

### Q1：为什么不把 Agent / LLM 调用直接写进 C++ Gateway？

Gateway 负责连接接入、限流、转发与 SSE 透传；Agent 负责规则决策、LLM 调用和 Tool 路由。这样可以避免 Python 业务逻辑影响接入层吞吐，也方便独立迭代 Agent 逻辑。

### Q2：为什么当前版本不做 `gRPC`？

MVP 阶段 `HTTP + JSON` 已足够支撑演示与压测，避免引入额外复杂度。

### Q3：为什么要做 `epoll` vs `io_uring` 并发模型对比？

不是只写功能，而是通过同机同脚本压测验证不同 I/O 模型在高连接压力下的表现。

### Q4：为什么主运行环境是 Linux VM？

`io_uring` 依赖 Linux 内核能力，压测也需要统一环境，Windows 只适合作为编辑环境。

## 11. 相关文档

- [TECH_DESIGN.md](TECH_DESIGN.md)：详细技术设计与接口说明
- [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md)：压测命令、原始数据与分析
- [DEMO_SCRIPT.md](DEMO_SCRIPT.md)：面试演示流程与讲解材料
- [TIL.md](TIL.md)：开发问题复盘与环境排障
- [PRD.md](PRD.md)：项目目标、边界与验收范围

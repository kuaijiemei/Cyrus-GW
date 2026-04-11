# AGENTS.md

本文件用于统一 Cyrus-GW 项目的协作方式、开发标准与落地流程。  
目标是：在 4 周内交付一个可演示、可压测、可讲解的双服务 MVP。

---

## 1) 项目概述

### 项目名称

`Cyrus-GW：C++20/io_uring 驱动的高并发 AI Gateway 与 Agent Runtime`

### 一句话定位

面向 LLM 场景构建高性能网关：基于 C++20 无栈协程与 io_uring 实现请求接入、异步调度、SSE 透传与限流控制；通过 Python Agent 完成任务决策与 Tool 调用闭环，并以同机基准测试验证并发模型性能差异。

### 项目本质

- 高性能网关（C++） + AI 执行系统（Python）
- 双服务架构，不是单体 Web 服务，不是纯 AI Demo

### 当前范围（MVP）

- 必做：`/chat`、请求转发、LLM 调用、基础限流、Agent tool 调用
- 可选：简单 memory、Redis（加分）
- 禁止：分布式、多 Agent 协作、`gRPC`、多租户、cost 计费、复杂策略引擎

---

## 2) 开发规范（协作约定）

### 文档优先级（冲突时按此顺序）

1. `PRD.md`
2. `TECH_DESIGN.md`
3. `RESEARCH.md`
4. 本文件 `AGENTS.md`

### 变更要求

- 新增功能前先确认是否超出 MVP 边界。
- 任何“架构级变更”必须同步更新 `PRD.md` 与 `TECH_DESIGN.md`。
- 不允许在未更新文档的情况下引入新基础设施（例如消息队列、复杂中间件）。
- 新增开发流程要求：每完成一个模块后，必须将本模块开发过程中遇到的问题沉淀到 `TIL.md`（包含现象、根因、解决方案、防复发措施、验证方式）。

### 任务拆分建议

- C++ Gateway 与 Python Agent 可并行开发。
- 先对齐 HTTP 接口契约，再并行实现。
- 每个任务提交前必须自测，至少覆盖：正常路径 + 错误路径。
- 每个模块完成后，必须更新一次 `TIL.md`，记录至少 1 条真实开发问题与解决复盘。

---

## 3) 开发环境

## 3.1 本地平台现状

- 当前主平台：Windows
- 无 WSL
- 可用远程 Linux 虚拟机

## 3.2 推荐方案（最简单）

**优先使用 Cursor Remote SSH 连接 RHEL 虚拟机开发，并在虚拟机内通过 Git 提交到 GitHub。**

原因：

- `io_uring` 依赖 Linux 内核能力，Windows 本地不适合作为主运行环境。
- 虚拟机内编译、运行、压测环境一致，结果更可信。
- 统一在虚拟机仓库提交，避免本地与远程双份代码漂移。

## 3.3 当前确认环境（已可用）

- OS：RHEL（Remote SSH 已连通）
- vCPU：4 核（Ryzen 4500U）
- RAM：15 GiB
- Kernel：`5.14.0`（支持 `io_uring` 生产级特性）
- 根目录可用空间：18 GiB（可用但需控制日志与构建产物）
- 共享目录：`/mnt/hgfs`（233 GiB，用于与 Windows 传输文件）

开发者视角评估：

- 并发开发能力：4 核 + 15 GiB 对当前 MVP 足够。
- `io_uring` 适配：Kernel 5.14.0 满足主线实现需求。
- 磁盘风险：根分区仅 18 GiB，可编译但必须定期清理 `build/`、日志和缓存。

## 3.4 远程 Linux 初始化（建议）

```bash
sudo dnf makecache
sudo dnf install -y gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config git curl python3 python3-pip
```

可选依赖（按实现需要安装）：

- `liburing` + `liburing-devel`（若直接使用 `io_uring`）
- `wrk` 或 `hey`（压测）

若执行 `dnf install cmake` 时出现「未找到匹配项」、且 `dnf repolist` 仅有少量第三方仓库：多为 **RHEL 未在订阅服务器注册**，`BaseOS` / `AppStream` 未启用。处理与临时绕过（用 `g++` 直接编网关）见根目录 `README.md` **§7.2.1**。

---

## 4) Windows -> Linux 跑通规划

## 4.1 工作流（推荐）

1. 在 Windows 中通过 Cursor Remote SSH 进入 RHEL 虚拟机工作区。
2. 在虚拟机目录中开发、构建、运行（不在 U 盘/FAT32 上做主开发）。
3. 在虚拟机目录中使用 Git 提交并推送 GitHub。
4. 仅将大文件或备份通过 `/mnt/hgfs` 与 Windows 互传。

## 4.2 最小运行流程（Linux VM）

### 启动 Python Agent

```bash
cd agent-py
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 -m uvicorn app.main:app --host 0.0.0.0 --port 8001
```

### 构建并启动 C++ Gateway

```bash
cd gateway-cpp
cmake -S . -B build -G Ninja
cmake --build build -j
./build/cyrus-gateway
```

### 本地验证

`-w '\n'` 避免响应与 shell 提示符粘在一行；需要可读 JSON 可加 `| python3 -m json.tool`。

```bash
curl -sS -w '\n' http://127.0.0.1:8080/health

curl -sS -w '\n' -X POST http://127.0.0.1:8080/chat \
  -H "Content-Type: application/json" \
  -d '{"message":"hello","stream":false}'
```

### Week 1 里程碑一键验收（TODO 1.5）

在仓库根目录执行（需已准备好 `agent-py/.venv`，见上文）：

```bash
bash scripts/verify_week1.sh
```

---

## 5) 测试要求

## 5.1 功能测试（必须）

- `/chat` 非流式成功
- `/chat` 流式 `SSE` 成功
- tool 调用路径至少 1 条可复现
- 限流触发返回 `429`
- LLM 超时触发“最多 1 次重试”

## 5.2 性能测试（必须）

需要完成两类压测：

1. 主实现：`Coroutines + io_uring`
2. 对照实现：`epoll + 线程池`（简化版即可）

至少输出：

- 并发：100 / 300 / 500
- 吞吐、平均延迟、P95/P99、错误率
- 流式场景 TTFT

## 5.3 回归测试（建议）

- 变更网关转发逻辑后，回归：正常请求、超时、上游错误、限流边界
- 变更 Agent 决策后，回归：直接回答路径、tool 路径、异常路径

---

## 6) 代码风格

## 6.1 C++ 代码风格

- 标准：C++20
- 命名建议：
  - 类型：`PascalCase`
  - 函数/变量：`snake_case` 或 `camelCase`（项目内保持一致）
  - 常量：`kPascalCase` 或 `UPPER_SNAKE_CASE`（保持一致）
- 规则：
  - 禁止阻塞 I/O 出现在请求主路径
  - 错误码与异常统一封装
  - 关键并发逻辑必须有注释（说明锁/无锁与线程安全边界）

## 6.2 Python 代码风格

- 参考 PEP 8
- FastAPI schema 必须类型化（Pydantic）
- Agent 决策输出必须结构化，避免弱字符串协议

## 6.3 日志规范（最低）

- 必含字段：
  - `request_id`
  - `latency_ms`
  - `status_code`
  - `tool_used`
- 建议增加：
  - `retry_count`
  - `queue_wait_ms`

---

## 7) 注意事项（高频坑位）

- 不要把 AI 逻辑塞到 C++ Gateway，Gateway 只做高并发接入和转发。
- 不要提前引入重型基础设施（例如 `gRPC`、服务网格、复杂消息队列）。
- 不要为了“看起来高级”而牺牲 4 周交付节奏。
- `shell_tool` 必须有白名单或受限命令集，避免安全风险。
- 所有外部 API key 一律走环境变量，不进仓库。

---

## 8) Definition of Done（完成标准）

满足以下条件才算任务完成：

1. 代码可在 Linux VM 编译运行。
2. 功能自测通过（对应模块至少 1 条主路径 + 1 条异常路径）。
3. 日志字段符合最低要求。
4. 若影响架构/流程，已同步更新文档。
5. 不引入 PRD 禁止项。
6. 模块开发过程中遇到的问题已沉淀到 `TIL.md`（每个已完成模块至少 1 条）。

---

## 9) 建议补充产物（可选但强烈建议）

- `INTERFACE_CONTRACT.md`：Gateway 与 Agent 的 HTTP 协议契约
- `BENCHMARK_PLAN.md`：压测命令、数据表、结论模板
- `DEMO_SCRIPT.md`：5 分钟面试演示脚本
- `TIL.md`：模块级开发问题与经验沉淀（建议长期维护）

这些文档会显著提升“可讲解性”和面试通过率。

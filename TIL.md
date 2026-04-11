# TIL（Today I Learned）- 开发问题与经验沉淀

> 目的：沉淀 Cyrus-GW 开发过程中遇到的真实问题、根因和可复用解法，避免重复踩坑。  
> 规则：每完成一个模块（例如 Gateway 路由、SSE、限流、Agent tool、压测对比）后，必须至少新增 1 条记录。  
> 范围：记录“开发中遇到的问题”，而不是单纯功能进度。

---

## 1) 记录规范（必填字段）

每条记录请按以下字段填写：

- 日期（YYYY-MM-DD）
- 模块（如 `gateway-cpp/sse`、`agent-py/tool_router`）
- 现象（实际报错/异常行为）
- 根因（为什么发生）
- 解决方案（做了什么修复）
- 防复发措施（如何避免再次出现）
- 验证方式（正常路径 + 异常路径）
- 关联文件（可选，建议写路径）

---

## 2) 记录模板（复制使用）

```markdown
### [YYYY-MM-DD] 模块：<模块名>

- 现象：
- 根因：
- 解决方案：
- 防复发措施：
- 验证方式：
  - 正常路径：
  - 异常路径：
- 关联文件：
```

---

## 3) 问题清单（按时间倒序追加）

### [2026-04-11] 模块：repo/README-week1（TODO 1.5）

- 现象：Week 1 验收时，从仓库根目录直接执行 `./build/cyrus-gateway` 或复制 README 片段却找不到可执行文件 / 读不到 `configs/gateway.yaml`；或运行 `verify_week1.sh` 时终端长时间不返回。
- 根因：文档默认 `cd gateway-cpp` 后构建，但读者常在错误 cwd 下运行；部分环境未安装 `cmake`，与 README 主路径不一致；旧版验收脚本用 `python3 <<'PY' &` 拉起 Mock，PID/子进程边界不清晰，叠加 `EXIT` trap 时偶发后台未干净退出，表现为脚本“挂住”。
- 解决方案：在 `README.md` §7 增补 `g++` 一键编译回退路径、强调在 `gateway-cpp` 启动或显式传配置文件；增加根目录 `scripts/verify_week1.sh` 指向 `verify_1_4.sh`；将 Mock 改为 `mktemp` 独立 `.py` 文件 + `wait` 收尾，缺网关二进制时脚本内自动 `g++` 编译，缺 `.venv` 时立即失败并提示命令；验收脚本内启动 `uvicorn` 前必须 `cd agent-py`，否则 `app` 包导入失败、首段 E2E 恒为 `502`。
- 防复发措施：里程碑脚本与 README 交叉引用同一脚本路径；后续若改构建方式需同步两处。
- 验证方式：
  - 正常路径：在仓库根执行 `bash scripts/verify_week1.sh`，应依次打印 200 / 400 / 502 三段验收输出。
  - 异常路径：故意在错误目录启动网关，确认仍可通过传参配置文件或 `TIL` 所述路径解析恢复。
- 关联文件：`README.md`、`scripts/verify_week1.sh`

### [2026-04-11] 模块：gateway-cpp/upstream-agent-forward（TODO 1.4）

- 现象：初版在验收脚本里“停掉 Agent”后，`/chat` 仍返回 200，误以为 Gateway 没有正确区分上游失败。
- 根因：脚本只 kill 了外层 shell 进程，`uvicorn` 子进程未真正退出，导致上游仍可用。
- 解决方案：在验收脚本补充 `pkill -f "uvicorn app.main:app --host 127.0.0.1 --port ..."`，确保 Agent 真正下线；Gateway 在连接拒绝时返回 `502` 并映射 `error_code=10004`、`error_layer="agent"`。
- 防复发措施：后续所有端到端脚本在模拟上游故障时必须同时验证端口监听是否消失，避免“假故障”。
- 验证方式：
  - 正常路径：Gateway 转发到 Agent + mock LLM，`POST /chat` 返回 200，body 为 Agent 非流式响应。
  - 异常路径：停止 Agent 后 `POST /chat` 返回 502，且 body 含 `error_layer="agent"`。
- 关联文件：`gateway-cpp/src/api/chat_handler.cpp`、`gateway-cpp/src/upstream/agent_client.cpp`、`gateway-cpp/scripts/verify_1_4.sh`

### [2026-04-11] 模块：agent-py/llm_client（TODO 1.3）

- 现象：`/agent/chat` 在未配置上游 LLM 时最初会直接抛异常，调用方只能看到 500，不利于定位配置问题。
- 根因：缺少对“无上游/无 API key”场景的显式分支，异常未映射为可读业务错误。
- 解决方案：在 `OpenAICompatibleLLMClient` 增加 `LLMConfigError` / `LLMTimeoutError` / `LLMUpstreamError`，并在 API 层映射为 `503/504/502` + 结构化 JSON（含 `error`、`error_code`、`detail`）。
- 防复发措施：把“未配置上游”纳入验收脚本固定检查项（`verify_1_3.sh`），每次改动 API 后都回归该场景。
- 验证方式：
  - 正常路径：`GET /health` 返回 200；配置真实上游后 `POST /agent/chat` 返回 200。
  - 异常路径：未配置 `LLM_BASE_URL` 时 `POST /agent/chat` 返回 503，且 body 包含 `llm_not_configured`。
- 关联文件：`agent-py/app/llm_client.py`、`agent-py/app/api.py`

### [2026-04-11] 模块：gateway-cpp/http_server（TODO 1.2）

- 现象：`POST /chat` 偶发或稳定解析失败、JSON 不完整；或编译报错 `optional` / `string_view` 未声明。
- 根因：仅 `recv` 到 `\r\n\r\n` 就当作完整请求时，`Content-Length` 对应 body 仍在 socket 中未读完，后续请求读串；C++20 使用 `std::optional` / `std::string_view` 需显式包含 `<optional>` / `<string_view>`。
- 解决方案：在 `read_http_request` 中解析 `Content-Length`，在头部之后的 body 上循环 `recv` 直至凑满长度（并限制上限）；在 `chat_handler.cpp` 等处补齐标准库头文件。
- 防复发措施：后续若引入 HTTP 库或 io_uring 读盘，仍保持「先收满声明长度再业务处理」的契约；新增翻译单元时以 `-Wall -Wextra -Wpedantic` 编译。
- 验证方式：
  - 正常路径：`curl -X POST http://127.0.0.1:8080/chat -H "Content-Type: application/json" -d '{"message":"hello","stream":false}'` 返回 `200` 且 body 为合法 JSON。
  - 异常路径：`message` 为空或缺省时返回 `400`；`bash gateway-cpp/scripts/verify_1_2.sh` 可复现多场景。
- 关联文件：`gateway-cpp/src/net/http_server.cpp`、`gateway-cpp/src/api/chat_handler.cpp`

### [2026-04-11] 模块：gateway-cpp/skeleton（1.1 仓库初始化）

- 现象：在 `gateway-cpp/build/` 下直接运行 `./cyrus-gateway` 时，若仅依赖当前工作目录下的 `configs/gateway.yaml`，常出现读不到配置；本机 `g++` 编译报错 `weakly_canonical` 不是 `std` 的成员。
- 根因：可执行文件工作目录与源码树中 `configs/` 的相对位置不一致；`std::weakly_canonical` 位于命名空间 `std::filesystem`，误写为 `std::`。
- 解决方案：在 `resolve_default_config_path` 中增加对 `/proc/self/exe` 相邻路径的解析（`../configs/gateway.yaml`），加载失败时回退 `GatewayConfigSnapshot` 默认值；将 `weakly_canonical` 修正为 `std::filesystem::weakly_canonical`。
- 防复发措施：文档中明确推荐从 `gateway-cpp` 目录启动或传入显式配置文件路径；合并 1.2 后可在 `--config` 帮助中再次强调。
- 验证方式：
  - 正常路径：`cd gateway-cpp && g++ ... -o build/cyrus-gateway && ./build/cyrus-gateway`，`curl http://127.0.0.1:8080/health` 返回 `200` 与 JSON。
  - 异常路径：删除或损坏配置文件时应仍能监听默认端口（回退配置），并打印 `failed to load config`。
- 关联文件：`gateway-cpp/src/common/config_loader.cpp`、`gateway-cpp/src/main.cpp`

### [示例] 模块：gateway-cpp/rate-limiter

- 现象：高并发压测下偶发未触发 `429`，请求直接通过。
- 根因：令牌补充逻辑在边界时间窗口存在竞态，导致瞬时令牌被重复消费。
- 解决方案：将令牌补充与消费放在同一临界区，修正时间片换算精度。
- 防复发措施：补充边界时间单测与 300 并发回归压测。
- 验证方式：
  - 正常路径：低并发下不误伤，正常请求通过。
  - 异常路径：高并发突发下稳定触发 `429`。
- 关联文件：`gateway-cpp/src/limiter/token_bucket.cpp`

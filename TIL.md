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

### [2026-04-18] 模块：release-doc-sync（v1.0.0 发布前）

- 现象：准备发布 `v1.0.0` 时，根文档之间出现多处“说明比实现更超前”的漂移：`README.md` / `TECH_DESIGN.md` 仍把独立调度队列、`queue_wait_ms`、`llm_call_latency_ms` 当作已落地能力；目录树遗漏实际源码文件；`DEMO_SCRIPT.md` 中个别口播把 `latency_ms` 说成响应字段、把 `time_tool` 路径说成“两次 LLM 调用”、把 tool 失败说成 `error_layer=tool`；构建回退命令也没有覆盖全部 `.cpp`。
- 根因：开发过程中功能、脚本和答辩材料是分阶段分别更新的，但缺少一次“按真实仓库树 + 真实日志字段 + 真实响应模型”的发布前总校对，导致计划态描述、历史草稿和当前实现混在一起。
- 解决方案：在打 tag 前集中同步 `README.md`、`TECH_DESIGN.md`、`PRD.md`、`DEMO_SCRIPT.md`、`BENCHMARK_RESULTS.md`：统一标记 `session_id/memory` 为预留未落地；把目录树和构建说明改成与当前仓库一致；把日志字段改成实际输出集合（`request_id/path/latency_ms/stream/tool_used/status_code/retry_count`，条件字段 `ttft_ms/error_layer`）；修正 demo 话术与基准结论中的不准确表述。
- 防复发措施：以后每次准备 release 或答辩前，固定执行一次“文档对码”清单：① 用 `find` 对目录树；② 用 `rg` 对日志字段和错误层级；③ 用 `git diff` 复查 README / TECH_DESIGN / DEMO 三类对外材料；④ 将“预留未落地”与“已交付”显式分栏，避免文档默认读成“都已实现”。
- 验证方式：
  - 正常路径：发布前检查文档时，目录树、接口示例、日志样例、demo 口播与当前仓库实现一致，不再出现“文档里有、代码里没有”或“代码有、文档没写”的情况。
  - 异常路径：若后续新增配置项或日志字段但只改了代码、没改文档，应能通过发布前的 `rg` / `git diff` 总校对被发现。
- 关联文件：`README.md`、`TECH_DESIGN.md`、`PRD.md`、`DEMO_SCRIPT.md`、`BENCHMARK_RESULTS.md`

### [2026-04-14] 模块：demo-script-verify（TODO 4.4）

- 现象：`verify_week4_4.sh` 第一版在首条 `check()` 调用后立即退出（exit code 1），只输出 1 条 PASS 就终止，后续 25 条检查全未执行。
- 根因：脚本使用了 `set -e`（遇错即退），同时计数器用 `((PASS++))`。当 `PASS` 从 0 递增时，`((0++))` 的返回值是 0（bash 算术表达式中 0 视为 false），导致 `set -e` 将其当作命令失败而提前退出。这是 bash 算术扩展与 `set -e` 的已知交互陷阱。
- 解决方案：将 `((PASS++))` 改为 `PASS=$((PASS + 1))`，后者始终返回 exit code 0，不触发 `set -e`。同理修改 `((FAIL++))`。
- 防复发措施：验收脚本中计数器操作一律使用 `VAR=$((VAR + 1))` 形式，避免 `((VAR++))` 与 `set -e` 的兼容性问题；新增脚本后至少跑一次空环境冒烟测试。
- 验证方式：
  - 正常路径：`bash scripts/verify_week4_4.sh` 输出 PASS=26 FAIL=0，全部检查项均执行。
  - 异常路径：故意删除 `DEMO_SCRIPT.md` 后执行脚本，对应检查项应报 FAIL 并继续执行后续项。
- 关联文件：`scripts/verify_week4_4.sh`、`DEMO_SCRIPT.md`

### [2026-04-14] 模块：benchmark-data-collection（TODO 4.3）

- 现象：使用 `bench_week4_2.sh` 采集 C=100/300/500 正式数据时，wrk 非流式在 C=300/500 档位输出 `0 requests`、latency 全零（`-nan%`），C=100 也仅 7 RPS 且 60% 以上超时。同时流式 TTFT 在 C=100 全部超时（0/200），但 C=300 反而能 600/600 通过——现象前后矛盾。
- 根因：两层叠加导致：① mock LLM 初版使用 Python `http.server.HTTPServer`（单线程阻塞），100+ 并发下串行处理导致排队时间远超 wrk 默认 2s 超时；② wrk 默认 `--timeout` 为 2s，而 Gateway→Agent→mock LLM 端到端延迟在 C=100 时已达 ~600ms×并发排队≈数秒。C=100 流式全超时但 C=300 通过的原因是测试顺序导致：C=100 流式排在 6 轮非流式 wrk 之后执行，系统此时处于积压状态；到 C=300 时积压已消化。
- 解决方案：① 将 mock LLM 改为 `ThreadingHTTPServer`（`socketserver.ThreadingMixIn + HTTPServer`），使其能并发处理请求；② 给 wrk 命令添加 `--timeout 10s`，避免在正常 Agent 排队延迟下误报超时；③ 同时添加 `--latency` 参数以输出 P50/P90/P99 分位数（§4.3 采集 P95/P99 的必要条件）。
- 防复发措施：后续涉及压测的 mock 服务统一使用 `ThreadingHTTPServer`（或 uvicorn 多 worker），避免 mock 成为瓶颈；wrk 的 `--timeout` 应与 Gateway 的 `agent_timeout_ms` 对齐（建议 ≥ 上游超时 × 1.5）。
- 验证方式：
  - 正常路径：开发者本机 `bash scripts/bench_week4_2.sh --mode=epoll --warmup-sec=5 --duration-sec=15`，C=100 wrk 输出 37.22 RPS、0 超时、0 Non-2xx；全部 TTFT 采样（C=100/300/500）100% 成功。
  - 异常路径：C=500 非流式 wrk 15s 采样全超时（91/91），确认瓶颈在 Agent uvicorn 单 worker + GIL，Gateway 连接维持能力完好。
- 关联文件：`scripts/bench_week4_2.sh`、`BENCHMARK_RESULTS.md`、`bench_epoll.log`、`bench_iouring.log`

### [2026-04-14][专项排障 / DEBUG] 模块：benchmark-command-solidification-ttft（TODO 4.2）

- 现象：`bash scripts/bench_week4_2.sh --mode=epoll --run=stream` 在执行 TTFT 采样时，出现两类明显异常：一类是 `成功/失败 : 0/100` 且失败样本全为 `status=502 error=http_error_502`；另一类是 `wrk` 显示 `0 requests`、TTFT 采样全部 `timed out`。同一仓库里直接单独运行 `python3 scripts/stream_ttft_bench.py ...` 又能在低并发下得到 `4/0` 成功，现象前后不一致。
- 根因：压测脚本初版只负责“打印并执行命令”，没有在执行前确保 mock LLM、Agent、Gateway 已就绪。结果是：
  - 当 `8001` 上没有 Agent 时，Gateway 转发流式请求立即返回 `502`；
  - 当 `8080` 上没有可用 Gateway 或脚本上一轮残留状态不一致时，非流式压测会表现为 `0 requests`，流式则表现为连接超时。
- 解决方案：在 `scripts/bench_week4_2.sh` 中补充 `ensure_services()`，执行压测前自动检查并拉起 mock LLM（`:19943`）、Agent（`:8001`）、Gateway（`:8080`）；若服务已在运行则直接复用，若是脚本自己拉起则在退出时通过 `trap` 清理。最终将“环境准备”纳入压测入口本身，而不是依赖人工先手动起服务。
- 防复发措施：
  - 将“压测前服务健康检查 + 自动拉起”固定为 `bench_week4_2.sh` 的标准行为，避免使用者忘记先起服务；
  - 保留 `--print-only` 作为纯命令查看入口，真正执行时统一走 `ensure_services()`；
  - 后续做 4.3 数据采集时，所有对比测试统一从空端口状态开始，先确认 `lsof -i :8080`、`lsof -i :8001` 无残留，再跑脚本，避免混入历史进程状态。
- 验证方式：
  - 正常路径：在 `8080/8001` 都未监听的前提下执行 `bash scripts/bench_week4_2.sh --mode=epoll --run=stream --levels=2 --duration-sec=5 --warmup-sec=3`，应看到脚本打印“自动拉起 mock LLM + Agent + Gateway（epoll）”，随后 TTFT 采样结果为 `成功/失败 : 4/0`，并给出 `TTFT avg/p50/p95/p99`。
  - 异常路径：执行 `bash scripts/bench_week4_2.sh --mode=invalid --print-only`，应立即失败并提示 `--mode 仅支持 iouring/epoll/blocking`。
- 关联文件：`scripts/bench_week4_2.sh`、`scripts/stream_ttft_bench.py`、`README.md`

### [2026-04-14] 模块：benchmark-command-solidification（TODO 4.2）

- 现象：Week 4.2 输出要求中写的是“新增 `scripts/verify_week3_2.sh` 一键验证脚本”，但仓库里该文件已被 Week 3.2（Tool 框架验收）占用；若直接覆盖会破坏已完成里程碑回归。
- 根因：脚本命名与历史里程碑编号冲突，属于“验收入口命名”层面的流程问题，而非功能代码问题。
- 解决方案：保留原有 `scripts/verify_week3_2.sh` 不变，新增 `scripts/verify_week4_2.sh` 作为 Week 4.2 专用入口，并在 `README.md` 明确两者职责边界（Week3 Tool 验收 vs Week4 压测命令固化验收）；同时新增 `scripts/bench_week4_2.sh`、`scripts/stream_ttft_bench.py`、`scripts/wrk_chat_non_stream.lua` 固化压测命令。
- 防复发措施：后续新增验收脚本统一采用 `verify_week<week>_<module>.sh` 命名，新增前先检索仓库已有脚本，避免覆盖历史验证入口。
- 验证方式：
  - 正常路径：`bash scripts/verify_week4_2.sh` 输出 `PASS=6 FAIL=0`，并显示 `DONE: Week 4.2 压测脚本与命令固化 verify passed`。
  - 异常路径：`bash scripts/bench_week4_2.sh --mode=invalid --print-only` 应立即失败并提示 `--mode 仅支持 iouring/epoll/blocking`。
- 关联文件：`scripts/bench_week4_2.sh`、`scripts/verify_week4_2.sh`、`scripts/stream_ttft_bench.py`、`scripts/wrk_chat_non_stream.lua`、`README.md`

### [2026-04-14] 模块：gateway-cpp/concurrent-model-prep（TODO 4.1）

- 现象：io_uring 服务（`--mode=iouring`）在 RHEL 9 虚拟机上启动时 `io_uring_setup` 返回 `EPERM`（errno=1），所有 worker 线程初始化失败；`epoll + 线程池` 模式可正常运行，`blocking` 模式回归通过。
- 根因：RHEL 9 / EL9 默认设置 `kernel.io_uring_disabled=2`（完全禁用 io_uring，含 root），这是上游 Red Hat 安全加固策略。即使 kernel 5.14 本身支持 io_uring，此 sysctl 仍会在 `io_uring_setup` 系统调用层面直接拒绝。
- 解决方案：在 `IoUringServer::run()` 启动前增加 `check_iouring_available()` 探测，使用一次性 `io_uring_setup(4, ...)` 测试成功后立即销毁。若探测失败，输出结构化诊断信息（`iouring_disabled_by_kernel`）和修复命令（`sudo sysctl -w kernel.io_uring_disabled=0`），不创建 listener 也不启动 worker，避免大量线程报错刷屏。验证脚本 `verify_week4_1.sh` 在检测到 `io_uring_disabled != 0` 时将 io_uring 运行时用例标为 SKIP 而非 FAIL，仅验证编译产物和诊断输出。
- 防复发措施：① CI/压测环境初始化脚本必须包含 `sysctl -w kernel.io_uring_disabled=0`；② 文档（README §7.2）增加"RHEL 9 io_uring 启用"步骤；③ 后续若做 Docker 化，在 Dockerfile 的 `CMD` 或 `entrypoint` 中检查 sysctl 并打印提示。
- 验证方式：
  - 正常路径：在 `io_uring_disabled=0` 环境下 `bash scripts/verify_week4_1.sh` 应输出 io_uring / epoll / blocking 三种模式全部 PASS（包含 /health + /chat e2e）。
  - 异常路径：在 `io_uring_disabled=2` 环境下同脚本应输出 io_uring SKIP + 诊断 PASS，epoll/blocking 全量 PASS，总结 FAIL=0。
- 关联文件：`gateway-cpp/src/net/iouring_server.cpp`、`gateway-cpp/src/net/uring_compat.h`、`gateway-cpp/src/net/epoll_server.cpp`、`gateway-cpp/src/main.cpp`、`scripts/verify_week4_1.sh`

### [2026-04-14] 模块：week3-milestone-verify（TODO 3.5）

- 现象：手工按顺序分别跑 `verify_week3_1.sh`、`verify_week3_2.sh`、`verify_week3_3.sh` 时，演示或回归容易漏跑其中一段，或中途失败后误以为「后面两段已通过」；对外统一说「Week3 过了」但缺少单一入口证据。
- 根因：Week3 验收分散在三个脚本，缺少与 Week2.5 同级的「总编排入口」；`bash scripts/verify_week3_1.sh` 这类相对路径在 cwd 不在仓库根时也会直接报「找不到文件」。
- 解决方案：新增 `scripts/verify_week3_5.sh`，用 `ROOT="$(cd .../.. && pwd)"` 固定仓库根，再以绝对路径串行 `bash "${ROOT}/scripts/verify_week3_{1,2,3}.sh"`；任一段非 0 即整里程碑失败（`set -e`），末尾打印统一 DONE 标记。
- 防复发措施：对外演示与 CI 回归 Week3 统一只跑 `bash scripts/verify_week3_5.sh`；子脚本改动后仍应可单独跑单段排障。
- 验证方式：
  - 正常路径：在仓库根执行 `bash scripts/verify_week3_5.sh`，应依次看到 3.1/3.2/3.3 段标题与各自 PASS 汇总，最后出现 `DONE: Week 3.5 milestone verify finished`。
  - 异常路径：故意破坏某段依赖（如停掉 venv），总脚本应在对应子段立即非 0 退出，便于定位是 3.1/3.2/3.3 哪一层。
- 关联文件：`scripts/verify_week3_5.sh`、`scripts/verify_week3_1.sh`、`scripts/verify_week3_2.sh`、`scripts/verify_week3_3.sh`

### [2026-04-14] 模块：tool-llm-feedback-loop-verify（TODO 3.3）

- 现象：`verify_week3_3.sh` 初版只校验了 HTTP 响应里的 `tool_used`，看起来“功能通过”，但无法证明“Agent 确实做了两次 LLM 调用（先决策、再最终回答）”，演示时仍会被追问“是不是只做了一次调用”。
- 根因：仅看最终响应缺少中间证据；`tool_used` 只能证明“工具被用过”，不能证明“tool 结果已经回填到最终 prompt 并触发二次 LLM 组织答案”。
- 解决方案：在 Mock LLM 侧增加 trace 记录（`decision` / `final_answer` 两阶段 JSONL），脚本验收时打印 `SHOW_DECISION_JSON` 与 `SHOW_FINAL_CONTEXT`，并断言最终上下文包含 `Tool 'time_tool' returned:`。
- 防复发措施：后续涉及“多阶段编排”的验收脚本，必须同时提供“结果断言 + 中间过程证据断言”；避免只看最终 HTTP 字段导致误判。
- 验证方式：
  - 正常路径：`bash scripts/verify_week3_3.sh` 输出 `TRACE_DECISION=2`、`TRACE_FINAL=2`，且 `SHOW_FINAL_CONTEXT` 含工具返回内容。
  - 异常路径：`force-bad-tool` 请求下 `tool_used=""` 且 Agent 进程存活，说明工具失败已降级、不崩溃。
- 关联文件：`scripts/verify_week3_3.sh`、`agent-py/app/agent_core.py`

### [2026-04-14] 模块：tool-framework-verify（TODO 3.2）

- 现象：`verify_week3_2.sh` 第一版在“HTTP 请求验收”阶段连续 3 个断言失败，但响应里实际上已经包含 `tool_used` 预期值；终端同时出现 `/scripts/verify_week3_2.sh: 行XXX: rg：未找到命令`。
- 根因：脚本断言依赖 `rg`，而当前 RHEL 环境下运行脚本时 PATH 不含 `rg`，导致命令不存在并触发 `if` 分支误判；这是脚本工具依赖问题，不是 Tool 框架逻辑问题。
- 解决方案：将断言从 `echo "$resp" | rg ...` 改为纯 Bash 子串匹配 `[[ "$resp" == *'"tool_used":"time_tool"'* ]]`，彻底移除对外部搜索工具的依赖。
- 防复发措施：验收脚本默认只依赖 bash/curl/python3 等基础命令；新增脚本前先检查“无额外工具依赖”原则，避免在最小环境中误报失败。
- 验证方式：
  - 正常路径：`bash scripts/verify_week3_2.sh` 输出 `PASS=7 FAIL=0`，并显示 `DONE Week 3.2 verify passed`。
  - 异常路径：Mock LLM 返回错误 tool 参数或未知 tool 时，响应仍为 200 且 `tool_used=""`，同时 Agent 进程持续存活。
- 关联文件：`scripts/verify_week3_2.sh`、`agent-py/app/tool_router.py`、`agent-py/app/tools/contracts.py`

### [2026-04-14] 模块：agent-py/decision-mechanism（TODO 3.1）

- 现象：验收脚本第一版 `check()` 函数使用 `grep -qF '"tool_used": "time_tool"'`（冒号后有空格），但 FastAPI 返回的是紧凑 JSON（`"tool_used":"time_tool"`，无空格），导致所有断言误报 FAIL，实际逻辑已正确。
- 根因：FastAPI 默认使用紧凑序列化（`separators=(',', ':')` 风格），而脚本 `check()` 中的期望字符串按 Python `json.dumps` 默认格式（含空格）书写，两者不一致，`grep -qF` 严格字符串匹配无法命中。
- 解决方案：在 `check()` 函数中增加二次匹配：`compact_expected="$(echo "${expected}" | sed 's/": /":/')"` 再 grep，同时兼容 pretty-print 与紧凑格式。
- 防复发措施：验收脚本中所有 JSON 字段断言统一用双格式兼容模式；或改用 `python3 -c "import json,sys; print(json.load(sys.stdin)['key'])"` 精确断言字段值，彻底避免格式依赖。
- 验证方式：
  - 正常路径：`bash scripts/verify_week3_1.sh` 输出 12/12 PASS，三个分支（规则/LLM辅助/兜底）均覆盖。
  - 异常路径：Mock LLM 返回无效 JSON 时，`tool_used=""` 且请求正常返回 200（不崩溃）。
- 关联文件：`agent-py/app/agent_core.py`、`agent-py/app/schemas.py`、`agent-py/app/tool_router.py`、`scripts/verify_week3_1.sh`

### [2026-04-13] 模块：week2-milestone-verify（TODO 2.5）

- 现象：单独运行 2.1/2.2/2.3/2.4 都通过，但里程碑演示时经常漏步骤或顺序混乱，导致“功能都做了却难一次讲清”；手工切换脚本还会出现“上一段残留进程未清理”引发端口占用误报。
- 根因：Week2 验收项分散在多个脚本入口，缺少统一串行编排；当演示中断后手动重试，容易跳过某个验证段或忘记检查最终完成标记。
- 解决方案：新增 `verify_week2_5.sh` / `verify_2_5.sh`，统一串行调用 2.1~2.4，并在每段前打印标题、末尾输出里程碑完成标识，降低演示路径分叉与人工失误概率。
- 防复发措施：以后 Week2 对外演示统一使用 `bash scripts/verify_week2_5.sh`；若某段失败，只修该段后重新跑总脚本，确保最终输出完整链路。
- 验证方式：
  - 正常路径：运行总脚本后应依次看到 2.1/2.2/2.3/2.4 标题与各段成功输出，最后出现 `done: Week 2 milestone verify finished`。
  - 异常路径：故意停掉某段依赖（如 mock/agent），总脚本应在对应子段失败处立即报错退出，便于快速定位模块。
- 关联文件：`scripts/verify_week2_5.sh`、`gateway-cpp/scripts/verify_2_5.sh`

### [2026-04-13] 模块：error-status-unification（TODO 2.4）

- 现象：联调时“状态码正确但排障慢”，常见表现是：客户端拿到 429/502/504 却不确定失败层级；部分网关错误体缺少 `request_id`，跨 Gateway/Agent 日志难串联。
- 根因：错误契约在多个出口分散定义。`/chat` 解析错误、限流错误、上游错误分别在不同函数返回，若其中任一出口漏掉 `request_id` 或 `error_layer`，就会出现“有错误、难定位”的问题。
- 解决方案：统一网关错误体字段（`error/error_code/error_layer/request_id`），429 也回填 `request_id`；Gateway 日志新增 `error_layer`，并在 `/chat` 错误路径明确标记 `gateway` 或 `agent`；Agent 日志在 LLM 异常路径补 `error_layer=llm`，形成 gateway/agent/llm 分层证据。
- 防复发措施：后续错误处理改动统一回归 `bash scripts/verify_week2_4.sh`，至少核对 3 个点：响应体有 `request_id`、状态码匹配预期、日志含失败层级。
- 验证方式：
  - 正常路径：非错误请求保持 200，不引入额外错误字段干扰。
  - 异常路径：`2.4` 脚本覆盖 `400/429/502/504`，并输出 gateway/agent 两侧日志，能直接定位失败层级。
- 关联文件：`gateway-cpp/src/net/http_server.cpp`、`gateway-cpp/src/api/chat_handler.cpp`、`gateway-cpp/src/common/logger.cpp`、`agent-py/app/api.py`

### [2026-04-13] 模块：timeout-retry-policy（TODO 2.3）

- 现象：联调超时重试时，容易误把“响应慢”当作“未重试”，或者把“最终成功”当成“未触发超时”；另外若只看最终 HTTP 状态，难确认是否发生了重试与重试次数。
- 根因：缺少可观测证据闭环。仅靠一次 curl 看不到中间尝试；mock 行为如果不区分“首次超时、再次成功”与“持续超时”，无法验证“最多 1 次重试”是否真正生效。
- 解决方案：在 Gateway 与 Agent 日志统一输出 `retry_count`；新增 `verify_2_3.sh` 固化四段场景（Gateway 超时重试成功/失败、LLM 超时重试成功/失败），并通过 mock `/stats` 显示调用次数，直接验证“重试 1 次且不无限重试”。
- 防复发措施：后续修改超时或重试策略时，必须执行 `bash scripts/verify_week2_3.sh`，同时核对 3 个证据：最终状态码、`retry_count`、mock 调用计数（应为 2 而非无限增长）。
- 验证方式：
  - 正常路径：`2.3-1` 与 `2.3-3` 返回 200，且 stats 中对应消息调用计数为 2（首次超时 + 一次重试成功）。
  - 异常路径：`2.3-2` 与 `2.3-4` 返回 504，stats 中对应消息调用计数仍为 2，证明未无限重试。
- 关联文件：`gateway-cpp/src/api/chat_handler.cpp`、`agent-py/app/llm_client.py`、`gateway-cpp/scripts/verify_2_3.sh`

### [2026-04-13] 模块：gateway-cpp/rate-limit-token-bucket（TODO 2.2）

- 现象：限流联调初期看起来“偶发不触发 429”，同一批请求在不同机器上结果波动大，容易误判为令牌桶实现有 bug。
- 根因：测试方式不稳定。若只改很小的 `capacity` 但保留较高 `refill_per_sec`，请求间隔和机器调度会不断补充令牌，导致 429 命中率抖动；另外未固定同一批 burst 请求规模，难以横向比较参数效果。
- 解决方案：把验收脚本固定为两组可重复参数：严格组 `capacity=2, refill_per_sec=0`（稳定触发 429），宽松组 `capacity=10, refill_per_sec=100`（显著减少 429）；统一用 6 次连续 POST 比较 `200/429` 统计，并输出网关日志命中行（`tool_used=rate_limited`）。
- 防复发措施：后续限流回归统一执行 `bash scripts/verify_week2_2.sh`，不再依赖人工手点请求；若改限流算法，必须保留“严格组触发 + 宽松组对照”两段验证。
- 验证方式：
  - 正常路径：宽松参数下 6 次请求应多数/全部 200。
  - 异常路径：严格参数下 6 次请求应稳定出现 429，且日志中有 `status_code=429`、`tool_used=rate_limited`。
- 关联文件：`gateway-cpp/src/limiter/token_bucket.cpp`、`gateway-cpp/src/net/http_server.cpp`、`gateway-cpp/scripts/verify_2_2.sh`

### [2026-04-13] 模块：gateway-cpp/sse-e2e（TODO 2.1）

- 现象：2.1 联调时出现三类误判：① `curl -N /chat` 输出里混入 `96`、`0` 等十六进制长度行；② 首包超时场景里 Agent 日志显示 200，但 Gateway 对客户端返回 504；③ 总超时中断与非流式回归在默认配置下不易稳定复现。
- 根因：① Gateway 初版按字节直透 Agent 响应体，未剥离 `Transfer-Encoding: chunked`；② 流式接口会先返回 HTTP 200 建立流，后续错误通过 SSE `event:error` 或网关超时映射体现，导致“Agent 200 / Gateway 504”并存；③ `sse.total_timeout_ms=120000` 默认过大，且测试脚本未固定“首包先到、第二包延迟”的输入模式，触发条件不稳定。
- 解决方案：在 `agent_client.cpp` 增加 chunked 解码器，先解出真实 SSE payload 再透传；统一 mock/验证脚本为真实换行 `\n\n`；补充 `verify_2_1.sh` 一键脚本，固化四段场景（正常流式、首包超时 504、总超时中断、非流式回归），并分别使用独立端口和超时参数确保可复现。
- 防复发措施：后续所有 SSE 验收统一执行 `bash scripts/verify_week2_1.sh`，并同时核对三项证据：客户端输出无 chunk 长度行、日志包含 `ttft_ms`、四段用例结果与预期一致（含首包 504 与总超时 `event:error`）。
- 验证方式：
  - 正常路径：`bash scripts/verify_week2_1.sh` 的 `2.1-1` 段可连续看到 `event: delta`，最后 `event: done`；`2.1-4` 段返回标准非流式 JSON。
  - 异常路径：同一脚本 `2.1-2` 段返回 HTTP 504（`detail=first_chunk_timeout`）；`2.1-3` 段先 `delta` 后 `event:error`（`stream_total_timeout`）。
- 关联文件：`gateway-cpp/src/upstream/agent_client.cpp`、`gateway-cpp/src/api/chat_handler.cpp`、`agent-py/app/api.py`、`gateway-cpp/scripts/verify_2_1.sh`、`scripts/verify_week2_1.sh`

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

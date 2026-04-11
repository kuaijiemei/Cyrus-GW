# Cyrus-GW 调研结论与架构建议

> 适用范围说明：本文是偏“中长期架构调研”的能力地图，包含部分企业级思路。当前 4 周单机 MVP 的实际实施范围以 `PRD.md` 为准。

> MVP 开发流程补充：无论参考本文的哪一项技术方案，模块落地完成后都必须将开发过程中的问题复盘写入 `TIL.md`（至少包含现象、根因、解决方案、防复发措施、验证方式）。

本文基于对 Cloudflare AI Gateway、Kong AI Gateway、Apache APISIX AI 插件的调研，聚焦三个核心痛点：SSE 转发、多租户限流、高可用切换，并给出 Cyrus-GW 的核心 Feature 选型建议。

---

## 1. 三大痛点的技术实现路径

### 1.1 SSE 转发：如何在不阻塞网关性能的前提下透传逐字回复

#### 业界方案共性

- 上游采用 `stream=true`（或等效参数）后，网关进入流式透传模式。
- 关键点不是“能不能转发”，而是“如何减少 buffering、避免阻塞和复制”。
- 成熟网关通常会在以下方面做约束：
  - 首包超时（TTFB / first token timeout）与整体连接超时分离；
  - 对 `SSE` chunk 进行边读边写；
  - 关闭或限制中间层 buffering；
  - 保持连接池与背压（backpressure）能力，防止慢客户端拖垮 worker。

#### 典型产品启发

- Cloudflare 侧重托管能力，强调首段响应及时返回后持续流式。
- Kong/APISIX 侧重可配置插件链，能与 routing/fallback/observability 联动。
- APISIX 生态明确提到可通过关闭 `proxy_buffering` 避免 `SSE` 被缓冲。

#### 对 Cyrus-GW 的实现路径建议

1. 数据面使用事件驱动 + `Coroutines`，将上游读取与下游写入拆成独立 awaitable task。
2. `SSE` chunk 采用零拷贝或少拷贝通道（ring buffer + scatter/gather write）。
3. 引入显式 backpressure：下游写阻塞时暂停上游读取，避免内存堆积。
4. 超时策略拆分为：
   - `first_byte_timeout`（首 token SLA）
   - `between_chunks_timeout`（流中断检测）
   - `total_stream_timeout`（上限兜底）
5. 提供“流式透传模式”与“聚合模式”双路径，默认优先透传。

---

### 1.2 多租户限流：如何统计不同用户 Token 消耗并实时限流

#### 业界方案共性

- 限流维度从传统 QPS 扩展到 `prompt_tokens`、`completion_tokens`、`total_tokens`、`cost`。
- 身份维度从 route/service 扩展到 tenant/user/consumer/model/provider。
- 存储策略通常有三级：
  - `local`：低延迟，弱一致；
  - `cluster`：强一致，性能开销高；
  - `redis`：工程平衡方案。

#### 典型产品启发

- Kong 的 token/cost 策略与多维 policy 匹配最完整，适合企业级治理。
- APISIX 的 token 级限流与 consumer 插件组合灵活，便于快速搭建多租户控制。
- Cloudflare 提供 token/cost analytics 与动态路由限制能力，但更偏平台化托管能力。

#### 对 Cyrus-GW 的实现路径建议

1. 统一“计量对象”：
   - `tenant_id`
   - `project_id`
   - `user_id`
   - `provider/model`
2. 统一“计量指标”：
   - `prompt_tokens`
   - `completion_tokens`
   - `total_tokens`
   - `cost_microunit`（内部统一微单位计费）
3. 请求生命周期分两段计费：
   - 预扣（admission control，估算上限或最小扣减）
   - 结算（收到 usage 后精确补差）
4. 限流策略支持组合表达式（AND/OR），例如：
   - tenant 级总额度 + user 级瞬时额度 + model 级特殊额度
5. 计数存储先落地 `redis`，并保留本地热缓存 + 异步对账机制。

---

### 1.3 高可用切换：后端 DeepSeek/OpenAI 故障时如何秒级自动重试/切换

#### 业界方案共性

- 标准链路是：`timeout/error -> retry(backoff) -> fallback -> circuit breaker`。
- 可配置触发条件通常包含：
  - network error
  - timeout
  - `http_429`
  - `http_5xx`
- 成熟实践会区分“可重试错误”与“不可重试错误”，防止无效重试放大故障。

#### 典型产品启发

- Cloudflare：重试与 fallback 配置简单，适合快速上线。
- Kong：failover criteria + circuit breaker 组合完整，适合复杂故障场景。
- APISIX：多实例优先级 + fallback strategy + health check，工程可控性高。

#### 对 Cyrus-GW 的实现路径建议

1. 引入 provider-instance 状态机：
   - `Healthy`
   - `Degraded`
   - `Open`（熔断打开）
   - `HalfOpen`
2. retry 策略细化：
   - 按错误类型配置最大重试次数；
   - 支持 `constant/linear/exponential backoff`；
   - 对流式请求默认只在“未首包前”重试。
3. fallback 规则支持优先级 + 权重：
   - 主模型失败后切同 provider 低配模型；
   - 再切跨 provider 备用模型。
4. 主动健康检查 + 被动熔断双轨并行，缩短故障感知时间。
5. 全链路打点：记录每次重试与 fallback step，供 SLO 与容量复盘。

---

## 2. Cyrus-GW 的 4 个核心 Feature 及选型依据

### Feature 1：高性能流式转发引擎（`io_uring` + `Coroutines` + `SSE`）

#### 定义

构建面向长连接与高并发流式输出的数据面引擎，支持 OpenAI-compatible `SSE` 逐 token 透传。

#### 选型依据

- `io_uring` 在 Linux 下可显著降低 syscall 开销，提升 I/O 并发能力。
- `Coroutines` 能将复杂异步状态机表达为线性逻辑，减少 callback 复杂度。
- 对 AI 网关最关键的体验指标（TTFT、token 间隔抖动）更友好。

#### 必要性

没有高效流式引擎，网关会成为模型响应链路的性能瓶颈，直接拉高首 token 延迟与尾延迟。

---

### Feature 2：Token/Cost 多维度配额中心（实时限流 + 精确计量）

#### 定义

基于 tenant/user/model/provider 的多维 policy 引擎，支持 token 与 cost 两类预算控制。

#### 选型依据

- Kong/APISIX 实践表明，AI 场景下 token/cost 比请求数更贴近真实资源占用。
- 企业场景需要“按人、按团队、按模型”的精细化成本治理。
- `redis` 作为共享计数后端在性能与一致性间具备成熟平衡点。

#### 必要性

若仅按 QPS 限流，无法防止大 prompt 或长输出导致的成本失控，也无法做到多租户公平。

---

### Feature 3：弹性路由与故障切换平面（Retry/Fallback/Circuit Breaker）

#### 定义

在单次请求生命周期内，支持可配置重试、跨模型 fallback、跨 provider 切换与熔断恢复。

#### 选型依据

- 业界三家都已验证：AI provider 抖动是常态，自动切换是刚需。
- circuit breaker 能避免“雪崩式重试风暴”。
- priority + weight 的路由模型兼顾稳定性、成本和质量。

#### 必要性

没有该能力，DeepSeek/OpenAI 任一波动都可能直接放大为业务故障，无法满足企业 SLA。

---

### Feature 4：统一协议抽象与可观测性（OpenAI-compatible API + Metrics/Tracing）

#### 定义

提供统一的 northbound API（兼容 OpenAI 风格），并在 southbound 层适配多 provider；同时输出完整观测数据。

#### 选型依据

- Kong/APISIX 都强调“协议统一 + 多 provider 接入”以降低迁移成本。
- 观测指标（TTFT、tokens、fallback step、error taxonomy）是调优与计费对账基础。
- 内部通信可逐步引入 `gRPC` 作为控制面或插件 sidecar 的高效通道。

#### 必要性

没有统一抽象会导致业务 SDK 深度绑定单一厂商；没有可观测性则无法定位延迟与成本异常。

---

## 3. 落地优先级建议（MVP 到增强版）

### Phase 1（MVP）

- OpenAI-compatible chat/completions 接口
- `SSE` 透传（含首包超时与背压）
- 基础 token 限流（tenant + user）
- 基础 retry + fallback（按 `http_429/http_5xx/timeout`）

### Phase 2（增强）

- cost 配额与预算策略
- circuit breaker + active health check
- 多 provider 动态权重路由
- 观测面板（TTFT、tokens、error/fallback）

### Phase 3（企业级）

- 策略 DSL（多维 policy 编排）
- 灰度发布与 A/B 路由
- 高级审计与合规模块
- 插件化扩展与 `gRPC` 控制平面增强

---

## 4. 技术挑战预估（8G C 盘、项目在 40G D 盘）

当前环境特征是“系统盘偏小 + 项目盘较大”，对 C++20 开发链有明显影响。以下是重点注意事项：

### 4.1 构建缓存与临时文件可能挤爆 C 盘

- 问题来源：
  - 编译器临时文件、链接中间产物、PDB/obj；
  - 包管理器缓存（如 vcpkg、conan）；
  - IDE 与 LSP 索引缓存；
  - `%TEMP%` 默认在 C 盘。
- 建议：
  - 将 build 目录固定在 D 盘（out-of-source build）；
  - 将依赖缓存目录迁移至 D 盘；
  - 定期清理旧构建目录与无效符号文件。

### 4.2 Debug 构建体积膨胀风险高

- C++20 模板与 `Coroutines` 会放大调试符号体积。
- 建议采用分层构建策略：
  - 日常开发：`RelWithDebInfo` 或裁剪模块的 Debug；
  - 全量 Debug 仅在定位复杂问题时启用；
  - 对第三方依赖尽量复用预编译产物。

### 4.3 第三方依赖与工具链安装路径管理

- 即使工程在 D 盘，部分工具默认仍写入 C 盘用户目录。
- 建议：
  - 统一工具链安装与缓存路径；
  - 明确 CI 与本地一致的依赖锁定策略；
  - 对大体积依赖进行版本冻结，避免频繁重复下载。

### 4.4 日志与观测数据可能持续侵占磁盘

- AI 网关若开启详细请求/响应日志，增长速度很快。
- 建议：
  - 默认只保留结构化摘要日志；
  - 对 payload 日志按采样率开启；
  - 增加日志轮转、压缩和保留天数策略；
  - tracing/span 数据本地落盘需设置硬上限。

### 4.5 测试数据与回放样本治理

- `SSE` 回放、压测样本、mock 响应通常体积大且增长快。
- 建议：
  - 将 test artifacts 统一放在 D 盘专用目录；
  - 使用分级保留（最近版本全量、历史版本采样）；
  - 对二进制样本做压缩归档。

### 4.6 对开发效率的总体影响评估

- 若不做路径治理，8G C 盘会成为最先触发的系统性瓶颈，表现为：
  - 编译/链接失败；
  - IDE 卡顿、索引异常；
  - 包管理器安装失败；
  - 系统更新或虚拟内存异常。
- 结论：应在项目早期就将“磁盘路径与缓存治理”纳入工程基线，与性能优化同等级管理。

---

## 结论

Cyrus-GW 若要在高性能 C++ 网关方向建立差异化，必须优先把以下能力做深：高性能 `SSE` 转发、Token/Cost 多租户治理、秒级故障切换、统一协议与可观测性。这四项既是对业界方案的吸收，也是构建自研竞争力的最小闭环。

# Memory Management 设计文档

本文档定义 Wuwe Agent Framework 的 Memory Management 模块设计。该模块同时覆盖短期记忆和长期记忆，包含设计目标、核心抽象、存储接口、上下文组装规则、运行时集成方式、实现路径和使用示例。

## 目标

Memory Management 模块用于在框架层提供统一的记忆记录、检索、筛选、压缩和注入能力。它的核心职责是在调用 LLM 前，根据当前请求和应用策略，组装出可控、可审计、可扩展的上下文。

模块目标如下：

- 为短期记忆和长期记忆提供统一的数据模型。
- 与现有 `conversation`、`message`、`llm_request`、`llm_agent_runner` API 保持兼容。
- 支持替换不同的存储实现，而不要求业务代码修改。
- 在 LLM 请求发送前提供确定性的上下文组装流程。
- 支持上下文预算控制，避免注入内容超过模型上下文限制。
- 支持敏感信息、临时信息和持久信息的显式保留策略。
- 第一版不依赖 embedding、向量数据库或外部服务即可工作。
- 为语义检索、摘要、持久化和业务自定义排序预留稳定扩展点。

模块不应在应用不可见的情况下做长期记忆写入、覆盖或删除。记忆选择、注入和保留行为必须可配置、可检查。

## 术语

### 短期记忆

短期记忆表示当前会话、当前 agent 执行过程或当前进程生命周期内有效的信息。

典型内容包括：

- 最近的 user / assistant 消息。
- tool call 和 tool result。
- 当前执行过程中的临时状态。
- 编排流程产生的中间结果。
- 当前会话早期消息的摘要。

短期记忆通常在会话结束、任务结束或进程结束后清理。

### 长期记忆

长期记忆表示可以跨会话、跨 agent 执行复用的信息。

典型内容包括：

- 用户偏好。
- 项目事实。
- 领域记录。
- 已完成任务的总结。
- 用户明确要求保留的稳定指令。

长期记忆由持久化存储实现负责保存。长期记忆必须带有作用域、来源、保留策略和安全控制相关信息。

### 工作记忆

工作记忆是短期记忆的一类，用于保存单次 agent run 内的临时状态。

典型内容包括：

- 路由流程中选中的 route。
- 当前计划。
- 后续步骤需要复用的 tool result。
- 编排逻辑产生的临时状态。

### 记忆存储

记忆存储负责添加、更新、删除和检索 memory records。存储实现可以是内存、文件、SQLite、向量数据库或业务数据库。

### 记忆上下文

记忆上下文是运行时协调对象，负责连接存储、策略、检索、排序和请求增强逻辑。应用和 runner 通常通过 `memory_context` 使用记忆能力。

### 上下文组装

上下文组装指在 LLM 请求发送前，选择合适的 memory records，并将其格式化后注入 `llm_request` 的过程。

## 总体架构

Memory Management 模块分为五层：

1. 核心数据模型。
2. 存储接口和存储实现。
3. 策略与排序。
4. 上下文组装。
5. 与 runner、tools、flow primitives 的集成。

推荐目录结构：

```text
include/wuwe/agent/memory/
  memory_context.hpp
  memory_record.hpp
  memory_store.hpp
  memory_policy.hpp
  memory_ranker.hpp
  memory_scope.hpp
  memory_budget.hpp
  memory_summary.hpp
  in_memory_store.hpp
  file_memory_store.hpp
  sqlite_memory_store.hpp
  lexical_memory_ranker.hpp

src/agent/memory/
  file_memory_store.cpp
  sqlite_memory_store.cpp
  lexical_memory_ranker.cpp
```

第一版建议实现：

- `memory_record`
- `memory_query`
- `memory_store`
- `in_memory_store`
- `memory_policy`
- `memory_context`
- `lexical_memory_ranker`
- request augmentation
- runner 集成

文件存储和 SQLite 存储可以在核心接口稳定后实现，但设计文档中应提前定义它们的行为边界，避免后续破坏应用代码。

## 核心数据模型

### Memory Kind

```cpp
namespace wuwe::agent::memory {

enum class memory_kind {
  conversation,
  working,
  summary,
  long_term,
  retrieved,
};

}
```

`memory_kind` 定义记录的用途。

- `conversation`：原始会话消息或由消息派生出的记录。
- `working`：当前 run 内的临时状态。
- `summary`：会话、任务或历史片段的压缩摘要。
- `long_term`：需要跨会话保留的持久记录。
- `retrieved`：外部检索系统返回的记录。

### Memory Visibility

```cpp
namespace wuwe::agent::memory {

enum class memory_visibility {
  visible,
  hidden,
};

}
```

`memory_visibility` 控制记录是否允许注入模型请求。

- `visible`：允许被注入 prompt。
- `hidden`：仅供应用、诊断、审计、排序使用，不直接发送给模型。

该设计与现有 `agent::core::message_visibility` 保持一致。

### Memory Scope

```cpp
namespace wuwe::agent::memory {

struct memory_scope {
  std::string tenant_id;
  std::string user_id;
  std::string application_id;
  std::string conversation_id;
  std::string agent_id;
};

}
```

所有长期记忆操作都应带有作用域。存储实现必须将 scope 作为查询契约的一部分。

字段说明：

- `tenant_id`：租户或组织标识。
- `user_id`：用户标识。
- `application_id`：应用标识。
- `conversation_id`：会话标识。
- `agent_id`：agent 标识。

类型层面不强制所有字段非空，但生产应用至少应提供 `application_id`，并提供 `tenant_id` 或 `user_id` 中的一个。

### Memory Record

```cpp
namespace wuwe::agent::memory {

struct memory_record {
  std::string id;
  memory_kind kind { memory_kind::working };
  memory_visibility visibility { memory_visibility::visible };

  std::string content;
  std::string summary;

  memory_scope scope;

  double score { 0.0 };
  int priority { 0 };

  std::chrono::system_clock::time_point created_at {};
  std::chrono::system_clock::time_point updated_at {};
  std::optional<std::chrono::system_clock::time_point> expires_at;

  std::map<std::string, std::string> metadata;
};

}
```

字段语义：

- `id`：稳定记录标识。为空时，存储实现可以生成。
- `kind`：记录类别。
- `visibility`：是否允许注入模型请求。
- `content`：完整记忆内容，或序列化后的结构化内容。
- `summary`：较短表示，用于预算受限时替代 `content`。
- `scope`：租户、用户、应用、会话和 agent 作用域。
- `score`：存储或 ranker 计算出的检索分数。
- `priority`：应用指定的优先级。其他条件相同时，高优先级记录先被选中。
- `created_at`：创建时间。
- `updated_at`：最后更新时间。
- `expires_at`：过期时间。为空表示不按时间自动过期。
- `metadata`：扩展字段，可存储来源、敏感等级、topic、tool 名称等信息。

推荐 metadata key：

```text
source
sensitivity
retention
topic
tool_name
message_role
conversation_turn
```

### Memory Query

```cpp
namespace wuwe::agent::memory {

struct memory_query {
  std::string text;
  memory_scope scope;

  std::vector<memory_kind> kinds;
  std::size_t limit { 8 };

  std::map<std::string, std::string> filters;
  bool include_expired { false };
};

}
```

`memory_query` 用于存储检索和排序。

规则：

- `kinds` 为空表示所有 kind 均可参与检索。
- `limit` 是最大返回数量，不保证一定返回该数量。
- 必须先应用 `scope` 再排序。
- `filters` 默认表示精确匹配过滤，除非具体存储实现声明了额外行为。
- 除非 `include_expired` 为 true，过期记录必须被排除。

## 存储接口

### 基础接口

```cpp
namespace wuwe::agent::memory {

class memory_store {
public:
  virtual ~memory_store() = default;

  virtual memory_record add(memory_record record) = 0;

  virtual std::optional<memory_record> get(
    const std::string& id,
    const memory_scope& scope) const = 0;

  virtual std::vector<memory_record> search(const memory_query& query) const = 0;

  virtual bool update(memory_record record) = 0;

  virtual bool erase(const std::string& id, const memory_scope& scope) = 0;

  virtual std::size_t clear(const memory_scope& scope) = 0;
};

}
```

存储实现职责：

- 当新增记录没有 `id` 时生成 `id`。
- 执行 scope 过滤。
- 默认排除过期记录。
- 保留 metadata。
- 在可行时按相关性降序返回记录。
- 普通查询未命中不应抛异常。

存储损坏、路径非法、数据库错误、序列化失败等不可恢复错误可以使用异常表示。后续如果框架引入 `expected` 风格结果类型，可以再调整错误表达方式。

### In-memory Store

第一版必须实现 `in_memory_store`。

```cpp
namespace wuwe::agent::memory {

class in_memory_store final : public memory_store {
public:
  memory_record add(memory_record record) override;
  std::optional<memory_record> get(
    const std::string& id,
    const memory_scope& scope) const override;
  std::vector<memory_record> search(const memory_query& query) const override;
  bool update(memory_record record) override;
  bool erase(const std::string& id, const memory_scope& scope) override;
  std::size_t clear(const memory_scope& scope) override;

private:
  mutable std::mutex mutex_;
  std::vector<memory_record> records_;
};

}
```

检索行为：

- 按 scope 过滤。
- 按 kind 过滤。
- 按 metadata 过滤。
- 过滤过期记录。
- 按词法匹配分数、priority、更新时间排序。

该存储适用于测试、示例、短生命周期 session，以及由应用自行负责持久化的场景。

当前实现使用互斥锁保护内部 `records_` 和 id 生成器，允许多个 runner 在同一进程内共享 store 做基本并发访问。该保证仅覆盖单进程内的数据竞争，不等价于跨进程锁或事务隔离。

### File-backed Store

`file_memory_store` 使用 JSON Lines 持久化 memory records。

每行一条记录：

```json
{"id":"...","kind":"long_term","content":"...","metadata":{"source":"user"}}
```

推荐构造函数：

```cpp
class file_memory_store final : public memory_store {
public:
  explicit file_memory_store(std::filesystem::path path);
};
```

实现要求：

- 构造时从文件加载记录。
- 新增记录使用 JSON Lines 追加。
- 更新和删除通过重写文件完成。
- 重写时使用临时文件，再执行原子替换。
- 保留未知 metadata key。
- 序列化格式应稳定，便于用户检查和迁移。

当前实现对单进程内访问加锁。`add()` 使用追加写，避免每次新增都全量重写文件；`update()`、`erase()`、`clear()` 会通过临时文件重写完整 JSON Lines 文件。该存储适用于本地 agent、示例、开发工具和小规模部署，不提供跨进程写入协调。

### SQLite Store

`sqlite_memory_store` 适用于本地生产级持久化。

推荐 schema：

```sql
CREATE TABLE IF NOT EXISTS memory_records (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,
  visibility TEXT NOT NULL,
  content TEXT NOT NULL,
  summary TEXT NOT NULL,
  tenant_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  application_id TEXT NOT NULL,
  conversation_id TEXT NOT NULL,
  agent_id TEXT NOT NULL,
  score REAL NOT NULL,
  priority INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  expires_at INTEGER,
  metadata_json TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_memory_scope
  ON memory_records (tenant_id, user_id, application_id, conversation_id, agent_id);

CREATE INDEX IF NOT EXISTS idx_memory_kind
  ON memory_records (kind);

CREATE INDEX IF NOT EXISTS idx_memory_expiry
  ON memory_records (expires_at);
```

后续可加入 FTS5 支持全文检索：

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS memory_records_fts
USING fts5(id UNINDEXED, content, summary);
```

当前实现提供可选 `sqlite_memory_store`。构建系统会在 `WUWE_ENABLE_SQLITE` 为 ON 时通过 CMake `find_package(SQLite3 QUIET)` 探测 SQLite3；找到可用开发库时定义 `WUWE_HAS_SQLITE=1` 并链接 `SQLite::SQLite3`，否则定义 `WUWE_HAS_SQLITE=0`，不影响默认构建。

第一版 SQLite 实现在 SQLite 侧执行 schema、scope 查询、增删改查和索引维护，并在 C++ 侧完成 kind、metadata、expiry 过滤以及词法排序。该选择保持 `memory_store` 接口简单，也便于后续加入 FTS5 或 embedding ranker。

启用要求：

- 安装 SQLite3 development package，使 CMake 能找到 `SQLite::SQLite3`。
- 配置时保持 `WUWE_ENABLE_SQLITE=ON`，或显式传入 `-DWUWE_ENABLE_SQLITE=ON`。

未启用时，`sqlite_memory_store` 类型仍可被包含，但构造和方法调用会抛出清晰的运行时错误。

## 排序

排序逻辑应尽量与存储解耦。

```cpp
namespace wuwe::agent::memory {

class memory_ranker {
public:
  virtual ~memory_ranker() = default;

  virtual std::vector<memory_record> rank(
    const memory_query& query,
    std::vector<memory_record> candidates) const = 0;
};

}
```

### Lexical Ranker

第一版提供 `lexical_memory_ranker`。

排序输入：

- `query.text` 与 `record.content` 的子串匹配。
- 查询 token 与内容 token 的重叠度。
- `record.priority`。
- `record.updated_at`。
- `record.score`。

推荐排序规则：

1. 在组装模型上下文时，先排除 hidden records。
2. 词法匹配分数更高的记录优先。
3. priority 更高的记录优先。
4. 更新时间更近的记录优先。
5. 使用 store 提供的 `score` 作为最后的 tie-breaker。

该设计允许后续加入 semantic ranker，而不改变存储接口。

当前 `memory_context` 持有一个可替换的 `memory_ranker`。store 负责 scope、kind、metadata、expiry 等基础过滤，并可以返回较宽的候选集；最终排序和 `query.limit` 裁剪由 ranker 完成。这样可以在不改变 `memory_store` 接口的情况下替换词法排序、embedding 排序或业务排序。

## 策略

`memory_policy` 控制选择、保留和注入行为。

```cpp
namespace wuwe::agent::memory {

struct memory_policy {
  std::size_t max_recent_messages { 12 };
  std::size_t max_working_records { 16 };
  std::size_t max_long_term_records { 8 };
  std::size_t max_summary_records { 2 };

  std::size_t max_memory_chars { 6000 };
  std::size_t max_memory_tokens { 1500 };
  std::size_t estimated_chars_per_token { 4 };
  std::size_t max_record_chars { 1200 };

  bool include_conversation { true };
  bool include_working { true };
  bool include_summaries { true };
  bool include_long_term { true };

  bool require_scoped_recall { true };
  bool require_scope_for_long_term { true };
  bool dedupe_request_messages { true };

  bool inject_as_system_message { true };
  std::string injection_header { "Relevant memory:" };
};

}
```

策略要求：

- 策略必须显式、可检查。
- 上下文组装必须遵守 `max_memory_chars`，并同时受 `max_memory_tokens * estimated_chars_per_token` 的估算预算限制。
- 单条记录必须遵守 `max_record_chars`。
- 长期记忆可通过 `include_long_term` 关闭。
- 默认情况下，召回必须带有有效 scope，长期记忆写入必须带有生产可用 scope。
- 默认情况下，已经存在于当前 `llm_request.messages` 中的会话消息不得再次注入 memory block。
- hidden records 不得被注入模型请求。

中文应用可以将默认 `injection_header` 改为：

```cpp
std::string injection_header { "相关记忆：" };
```

## Memory Context

`memory_context` 是应用侧和 runner 集成侧使用的主要 API。

```cpp
namespace wuwe::agent::memory {

class memory_context {
public:
  memory_context();

  explicit memory_context(memory_policy policy);

  memory_context(
    std::shared_ptr<memory_store> short_term_store,
    std::shared_ptr<memory_store> long_term_store,
    memory_policy policy = {},
    std::shared_ptr<memory_ranker> ranker = {});

  memory_record remember(memory_record record);

  memory_record remember_working(
    std::string content,
    std::map<std::string, std::string> metadata = {});

  memory_record remember_long_term(
    std::string content,
    memory_scope scope,
    std::map<std::string, std::string> metadata = {});

  void observe(const chat_message& message, const memory_scope& scope = {});

  std::vector<memory_record> recall(const memory_query& query) const;

  llm_request augment(llm_request request, std::string_view query_text) const;

  const memory_policy& policy() const noexcept;
  void set_policy(memory_policy policy);

  void set_ranker(std::shared_ptr<memory_ranker> ranker);

  const memory_scope& scope() const noexcept;
  void set_scope(memory_scope scope);
};

}
```

存储使用规则：

- `conversation`、`working`、`summary` 默认写入短期存储。
- `long_term` 默认写入长期存储。
- `retrieved` 通常由外部检索系统返回，应用可按需要写入短期或长期存储。
- 如果只提供一个 store，可以将它同时用于所有 kind。
- 默认 ranker 是 `lexical_memory_ranker`；应用可以通过构造函数或 `set_ranker()` 替换为 semantic ranker、业务 ranker 或混合 ranker。
- 当 `policy.require_scope_for_long_term` 为 true 时，长期记忆写入要求 scope 至少包含 `application_id`，并包含 `tenant_id` 或 `user_id`。

### observe

`observe()` 将 `chat_message` 记录为短期会话记忆。

映射规则：

- `chat_message.role` 写入 metadata key `message_role`。
- `chat_message.name` 存在时写入 metadata key `name`。
- `chat_message.tool_call_id` 存在时写入 metadata key `tool_call_id`。
- `chat_message.content` 写入 `memory_record.content`。
- `memory_record.kind` 设置为 `memory_kind::conversation`。

空 content 的消息可以在包含 tool calls 时被记录。第一版可以跳过完全为空的消息。

### recall

`recall()` 根据 query 从短期和长期存储中检索记录。

推荐行为：

1. 如果 `policy.require_scoped_recall` 为 true，先确认 query scope 至少包含 `tenant_id`、`user_id` 或 `conversation_id` 中的一个。
2. 查询短期存储。
3. 查询长期存储。
4. 合并结果。
5. 按 id 或内容去重。
6. 将候选交给 `memory_ranker` 排序。
7. 返回最多 `query.limit` 条记录。

### augment

`augment()` 返回增强后的 `llm_request`。

必要行为：

1. 根据 `query_text` 构造 `memory_query`。
2. 按 policy 检索候选记录。
3. 排除 hidden records。
4. 如果 `policy.dedupe_request_messages` 为 true，排除已经存在于当前 `llm_request.messages` 中的会话消息。
5. 将记录格式化为 memory block。
6. 将 memory block 插入 request。
7. 返回修改后的 request。

推荐注入格式：

```text
Relevant memory:
- [long_term] User prefers concise C++20 examples.
- [summary] Earlier discussion selected runner-level integration.
- [working] Current task is to design Memory Management.
```

中文应用可以使用：

```text
相关记忆：
- [long_term] 用户偏好简洁、明确的 C++20 API。
- [summary] 前一轮讨论确定优先实现 runner 集成。
- [working] 当前任务是设计 Memory Management 模块。
```

如果 `inject_as_system_message` 为 true，应将 memory block 作为 system message 插入已有 system messages 之后、第一个 user message 之前。如果找不到合适位置，则插入 request 开头。

如果 `inject_as_system_message` 为 false，可以作为额外上下文消息注入：

```text
本次请求的相关上下文：
...
```

默认推荐 system message，因为 memory 是框架提供的上下文，不是新的用户输入。

## 与现有 Conversation 集成

现有 `agent::core::conversation` 应保持有效，不应被 memory 模块替代。

推荐新增自由函数：

```cpp
namespace wuwe {

void observe(agent::memory::memory_context& memory,
  const agent::core::conversation& conversation,
  const agent::memory::memory_scope& scope = {});

}
```

该函数遍历 `conversation.messages()`，将每条消息转换为短期会话记忆。

相比在 `conversation` 类中直接增加成员函数，自由函数对现有 core API 的侵入更小，更符合当前 Wuwe 的轻量设计。

## 与 LLM Agent Runner 集成

`llm_agent_runner` 应支持可选 memory，不改变现有行为。

推荐构造函数：

```cpp
class llm_agent_runner {
public:
  explicit llm_agent_runner(
    llm_client& client,
    agent::memory::memory_context* memory,
    int max_tool_rounds = 4);
};
```

推荐行为：

- `memory` 为空时，runner 行为完全保持现状。
- 第一次模型请求前，先调用 `memory->augment(request, query_text)`，再将原始 request messages 写入短期记忆，避免本轮 user message 被立即召回并重复注入。
- 每次 assistant response 返回后，调用 `memory->observe(...)`。
- 每次 tool result 返回后，调用 `memory->observe(...)`。
- tool results 默认作为短期会话记忆保存。

runner 不应自动写入长期记忆。长期记忆写入必须由应用显式调用，或通过应用主动暴露的 memory tool 完成。

## Memory Tools

当应用希望模型显式读写记忆时，可以将记忆能力封装成 reflected LLM tools。

示例：

```cpp
struct save_memory {
  static constexpr std::string_view description =
    "Save a durable memory record for future conversations.";

  std::string content;
  std::optional<std::string> topic;

  std::string invoke() const {
    // Application binds this tool to a memory_context instance.
    return "saved";
  }
};

struct search_memory {
  static constexpr std::string_view description =
    "Search durable memory records relevant to a query.";

  std::string query;
  int limit { 5 };

  std::string invoke() const {
    // Application binds this tool to a memory_context instance.
    return "[]";
  }
};
```

tool-based 长期记忆写入必须是 opt-in。框架不应默认让模型自主写入长期记忆，否则可能导致错误记忆、敏感信息保留或保留策略失控。

## 短期记忆实现

短期记忆第一版由 `memory_context` 持有的 `in_memory_store` 实现。

默认行为：

- 最近会话消息记录为 `memory_kind::conversation`。
- tool results 记录为 `memory_kind::conversation`，metadata 中 `message_role=tool`。
- 应用临时状态记录为 `memory_kind::working`。
- 摘要记录为 `memory_kind::summary`。
- 除非应用提供持久化短期存储，否则短期记录不持久化。

### 最近消息选择

最近消息优先于词法检索，因为它们表达当前会话连续性。

算法：

1. 选择当前 `conversation_id` 下 visible 的 `conversation` records。
2. 按 `created_at` 升序排序。
3. 保留最后 `policy.max_recent_messages` 条。
4. 应用上下文预算限制。

### 工作记忆选择

工作记忆按 priority 和更新时间选择。

算法：

1. 选择当前 scope 下 visible 的 `working` records。
2. 按 priority 降序排序。
3. priority 相同时按 `updated_at` 降序排序。
4. 最多保留 `policy.max_working_records` 条。
5. 应用上下文预算限制。

### 摘要选择

摘要记录应优先于更早的原始会话消息。

算法：

1. 选择当前 scope 下 visible 的 `summary` records。
2. 按 `updated_at` 降序排序。
3. 最多保留 `policy.max_summary_records` 条。
4. 应用上下文预算限制。

## 长期记忆实现

长期记忆应使用应用提供的持久化 store。

默认行为：

- 不自动写入长期记忆。
- 应用通过 `remember_long_term()` 或专门的 memory tool 写入。
- 长期记录必须带生产可用 scope。默认策略要求 `application_id` 非空，并且 `tenant_id` 或 `user_id` 至少一个非空。
- 检索由 query 和 policy 控制。

### 长期写入规则

应用只应持久化满足以下条件之一的信息：

- 用户偏好。
- 稳定的项目事实或领域事实。
- 用户明确要求记住的信息。
- 未来可能复用的已完成任务总结。

应用应避免持久化：

- secrets。
- access tokens。
- 原始凭据。
- 未审核的 tool outputs。
- 应用未明确支持的敏感个人信息。
- 只对单次任务有效的临时事实。

### 长期检索规则

长期检索必须先应用 scope。默认策略下，空 scope 不会召回任何记录。

算法：

1. 根据当前 user input 构造 query。
2. 使用 active scope 查询长期存储。
3. 过滤 kind 为 `long_term` 的记录。
4. 根据 metadata 过滤。
5. 执行排序。
6. 最多保留 `policy.max_long_term_records` 条。
7. 应用上下文预算限制。

### 长期更新规则

当新的长期信息与已有记录冲突时，应用应选择以下操作之一：

- 更新已有记录。
- 新增一条带有更晚 timestamp 的记录。
- 将旧记录标记为过期。
- 如果两条记录属于不同 scope 或 topic，则同时保留。

框架不应仅根据内容相似度静默覆盖长期记忆。

## 上下文预算

当前实现使用字符数预算，并提供轻量 token 估算预算。实际可用预算取 `policy.max_memory_chars` 和 `policy.max_memory_tokens * policy.estimated_chars_per_token` 中较小者。该方式不依赖具体 tokenizer，但能给模型上下文一个更保守的上限。

预算算法：

1. 初始化 `remaining = min(policy.max_memory_chars, policy.max_memory_tokens * policy.estimated_chars_per_token)`。
2. 优先加入 summary records。
3. 其次加入 long-term records。
4. 再加入 working records。
5. 最后加入 recent conversation records，但不重复注入已经在 `llm_request.messages` 中存在的消息。
6. 对每条记录：
   - 如果 `summary` 存在且比 `content` 短，优先使用 `summary`。
   - 否则使用 `content`。
   - 如果内容超过 `policy.max_record_chars`，按截断规则处理。
   - `remaining` 不足时停止。

推荐截断规则：

- 优先使用 `summary`，避免截断 `content`。
- 必须截断时，在字符边界截断并追加 `...`。
- 不应截断结构化 JSON，除非该记录提供了 summary。

## 注入格式

记忆注入格式应保持确定性，便于测试断言。

推荐格式：

```text
相关记忆：
- [summary] 前一轮讨论确定第一阶段实现短期记忆。
- [long_term] 项目使用 C++20 和 CMake。
- [working] 当前 route 是 Infrastructure Team。
```

规则：

- 每条记录一行。
- 包含 `memory_kind`。
- 默认不包含 id。
- 不包含 hidden records。
- 默认不包含 metadata。
- 记录内部换行需要规范化。

## 示例：短期记忆

```cpp
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "inclusionai/ling-2.6-1t:free",
    .timeout = 30000,
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  wuwe::agent::memory::memory_context memory;

  memory.observe({ .role = "user", .content = "We are designing Memory Management for Wuwe." });
  memory.observe({ .role = "assistant", .content = "The design should support short-term memory." });

  auto request = wuwe::make_message()
    << ("user" <wuwe::says> "List the required short-term memory components.");

  request = memory.augment(std::move(request), "short-term memory components");

  const auto response = client->complete(request);
  wuwe::println("{}", response.content);
}
```

预期注入内容：

```text
相关记忆：
- [conversation] We are designing Memory Management for Wuwe.
- [conversation] The design should support short-term memory.
```

## 示例：长期记忆

```cpp
#include <wuwe/agent/memory/file_memory_store.hpp>
#include <wuwe/agent/memory/in_memory_store.hpp>
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

int main() {
  auto short_term = std::make_shared<wuwe::agent::memory::in_memory_store>();
  auto long_term = std::make_shared<wuwe::agent::memory::file_memory_store>("memory.jsonl");

  wuwe::agent::memory::memory_policy policy {
    .max_recent_messages = 8,
    .max_long_term_records = 4,
    .max_memory_chars = 3000,
  };

  wuwe::agent::memory::memory_context memory(short_term, long_term, policy);

  wuwe::agent::memory::memory_scope scope {
    .user_id = "local-user",
    .application_id = "wuwe-example",
    .conversation_id = "design-session",
    .agent_id = "architect",
  };

  memory.remember_long_term(
    "The user prefers C++20 APIs with explicit ownership and minimal hidden behavior.",
    scope,
    { { "topic", "api-style" }, { "source", "user" } });

  auto request = wuwe::make_message()
    << ("user" <wuwe::says> "Design the public API for memory stores.");

  request = memory.augment(std::move(request), "memory store public API");

  const auto response = client->complete(request);
  wuwe::println("{}", response.content);
}
```

预期注入内容：

```text
相关记忆：
- [long_term] The user prefers C++20 APIs with explicit ownership and minimal hidden behavior.
```

## 示例：Runner 集成

```cpp
#include <wuwe/agent/memory/memory_context.hpp>
#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", {
    .base_url = "https://openrouter.ai/api",
    .model = "inclusionai/ling-2.6-1t:free",
    .timeout = 30000,
  });

  wuwe::agent::memory::memory_context memory;

  memory.remember_working(
    "Current implementation target is the Memory Management module.",
    { { "topic", "current-task" } });

  wuwe::llm_agent_runner runner(*client, &memory);

  const auto response = runner.complete("Summarize the next implementation step.");
  wuwe::println("{}", response.content);
}
```

runner 行为：

- prompt 会被相关 memory 增强。
- user prompt 会作为短期 conversation memory 记录。
- assistant response 会作为短期 conversation memory 记录。
- 启用 tool call 时，tool call 和 tool result 会作为短期 conversation memory 记录。

## 示例：Flow 集成

Flow 集成应在 `memory_context` 和 runner 集成稳定后实现。

候选 API：

```cpp
auto flow =
  make_request
  | wuwe::augment_memory(memory)
  | client;
```

`augment_memory` 是一个 flow step，接收 `llm_request`，调用 `memory.augment()`，并返回增强后的 request。

如果需要传入显式 query text，可以提供重载：

```cpp
auto flow =
  make_request
  | wuwe::augment_memory(memory, "incident routing request")
  | client;
```

该形式比 `with_memory` 更直接，因为它明确表达当前步骤是在增强 request，而不是改变整个 flow 的执行语义。

## 示例：Memory Tools

```cpp
struct remember_preference {
  static constexpr std::string_view description =
    "Store a durable user preference for future conversations.";

  std::string preference;
  std::optional<std::string> topic;

  std::string invoke() const {
    wuwe::agent::memory::memory_scope scope {
      .user_id = "local-user",
      .application_id = "wuwe-example",
      .agent_id = "assistant",
    };

    memory.remember_long_term(preference, scope, {
      { "topic", topic.value_or("preference") },
      { "source", "tool" },
    });

    return "Memory saved.";
  }

  wuwe::agent::memory::memory_context& memory;
};
```

该示例展示预期行为。实际工具绑定需要遵守 Wuwe reflected tool 的构造规则。如果 reflected tool type 不支持引用成员，应使用捕获 `memory_context` 的应用级 tool provider。

## 保留与过期

每条 memory record 可以包含 `expires_at`。

推荐保留行为：

- 普通检索默认排除过期记录。
- 存储可以保留过期记录，直到应用执行 compaction。
- 后续可以增加 `compact()` 操作删除过期记录。
- 长期记忆不应默认无限期保留，除非应用有明确需求。

推荐 metadata 值：

```text
retention=session
retention=project
retention=user
retention=temporary
```

框架不定义法律或合规政策。框架只提供应用实现自身政策所需的字段和 hook。

## 隐私与安全控制

模块应支持应用层隐私控制。

必要控制：

- 显式 scope 字段。
- hidden records。
- expires_at。
- sensitivity metadata。
- 长期记忆 opt-in 写入。
- 按 scope 清理记忆。

推荐 sensitivity metadata：

```text
sensitivity=public
sensitivity=internal
sensitivity=private
sensitivity=secret
```

默认行为：

- `sensitivity=secret` 的记录不应注入 prompt。
- `visibility=hidden` 的记录不得注入 prompt。
- 默认禁用自动长期持久化。

## 测试策略

单元测试应覆盖：

- 向 `in_memory_store` 添加记录。
- 按 scope 检索。
- 按 kind 检索。
- 过滤过期记录。
- augmentation 时过滤 hidden records。
- 词法匹配排序。
- 应用 `max_memory_chars`。
- 应用 `max_record_chars`。
- 在已有 system messages 后注入 memory。
- 保留原始 request messages。
- 将 chat messages observe 为 conversation memory。
- 同时从短期和长期 store recall。
- 空 scope 默认不召回记录。
- 长期记忆默认要求生产可用 scope。
- `visibility=hidden` 和 `sensitivity=secret` 不注入 prompt。
- 已存在于当前 request 的会话消息不重复注入。
- token 估算预算会限制 memory block。
- `memory_ranker` 可以被替换。
- `file_memory_store` reload 后继续递增生成 id。

集成测试应覆盖：

- 未启用 memory 的 `llm_agent_runner`。
- 启用 memory 的 `llm_agent_runner`。
- tool result observation。
- 从持久化 store 检索长期记忆。

测试不应调用真实 LLM provider。应使用 fake `llm_client` 捕获最终 `llm_request`。

当前仓库提供 `tests/memory_tests.cpp`，通过 CTest 注册为 `memory_tests`。该测试覆盖 scope 隔离、长期 scope 校验、hidden/secret 过滤、request 去重、预算与 ranker、文件存储 reload/id 递增，以及 runner 观察 assistant response 的链路。

当 `WUWE_HAS_SQLITE=1` 时，`memory_tests` 还会覆盖 SQLite store 的 add/get/search/update/erase/clear、metadata 过滤、expired 过滤和 reload 后 id 递增；当 SQLite 未启用时，测试会确认 `sqlite_memory_store` 返回明确的不可用错误。

运行方式：

```sh
cmake --build build --config Debug --target memory_tests
ctest --test-dir build -C Debug --output-on-failure
```

## 实现计划

推荐实现顺序：

1. 新增 `include/wuwe/agent/memory/memory_record.hpp`。
2. 新增 `include/wuwe/agent/memory/memory_store.hpp`。
3. 新增 `include/wuwe/agent/memory/in_memory_store.hpp`。
4. 新增 `include/wuwe/agent/memory/memory_policy.hpp`。
5. 新增 `include/wuwe/agent/memory/memory_context.hpp`。
6. 增加 store、recall、augmentation 测试。
7. 为 `llm_agent_runner` 增加接收 `memory_context*` 的可选构造函数。
8. 使用 fake client 增加 runner 测试。
9. 新增 `examples/src/memory_example.cpp`。
10. 实现持久化 store。
11. 在 direct API 稳定后增加 flow primitive。

## 当前状态与下一步路线

当前 Memory Management 已完成框架级最小稳定闭环：

- 核心数据模型、策略、存储接口和 `memory_context` 已实现。
- `in_memory_store`、`file_memory_store` 和可选 `sqlite_memory_store` 已实现。
- `llm_agent_runner` 已支持可选 memory，并避免当前请求消息重复注入。
- 默认安全策略已覆盖 scoped recall、长期记忆 scope 校验、hidden/secret 过滤和显式长期写入。
- `memory_ranker` 已接入，当前默认使用 `lexical_memory_ranker`。
- CTest 已覆盖 store、recall、augmentation、runner observation、文件持久化和 SQLite 持久化路径。

下一步应从“框架能管理记忆”推进到“agent 能显式使用记忆”。推荐按以下顺序推进：

1. **Memory tools**

   实现并测试 `save_memory` 和 `search_memory` 工具，让模型可以通过显式 tool call 保存和检索长期记忆。长期写入必须继续保持 opt-in，并经过应用绑定的 `memory_context` 和 scope 策略。第一版工具应优先支持：

   - 保存用户明确要求记住的偏好或稳定事实。
   - 按当前 scope 搜索长期记忆。
   - 拒绝空 scope 或不满足长期记忆 scope 策略的写入。
   - 对 `sensitivity=secret` 或应用标记的敏感内容默认拒绝持久化。

2. **SQLite memory example**

   新增一个真实示例，展示如何创建 SQLite-backed `memory_context`，写入长期记忆，经过 runner 增强请求，并在重启后重新加载同一份记忆。该示例不应调用真实 LLM provider，或应提供 fake client 版本，保证用户能本地验证。

3. **Conversation summarization**

   增加可选摘要压缩流程，把较早的 `conversation` records 压缩为 `summary` records，避免短期记忆无限增长。第一版可以要求应用提供 summarizer callback，框架只负责触发、保存和预算选择，避免默认隐式调用 LLM。

4. **SQLite retrieval upgrade**

   在 SQLite store 上增加 FTS5 或可插拔 semantic retrieval。优先保持 `memory_store` 接口不变，把更强召回能力放在 store 内部或 ranker 实现中。FTS5 可以作为第一步，embedding/向量检索作为后续扩展。

5. **Memory inspection and deletion API**

   提供面向应用和用户界面的审计能力，包括按 scope 列出记忆、查看 metadata、删除单条记录、按 scope 清理、按 sensitivity/topic 过滤。该能力是长期记忆进入真实产品前的必要控制面。

V1 Stable 的建议验收标准：

- Memory tools 有测试覆盖，不会绕过长期记忆 scope 策略。
- SQLite-backed 示例可以完整演示跨进程重启后的长期记忆复用。
- 摘要能力至少支持应用自定义 callback，且不会在未配置时自动调用 LLM。
- 用户或应用可以检查和删除长期记忆。
- 所有新增行为均通过 CTest 覆盖，且不改变未启用 memory 时的 runner 行为。

## 兼容性要求

Memory Management 模块必须保持现有行为：

- 现有 `llm_agent_runner` 构造函数继续有效。
- 现有 `llm_client::complete(llm_request)` 行为不变。
- 现有 flow examples 不受影响。
- 现有 `conversation` API 不受影响。
- 除非应用创建并传入 `memory_context`，否则不会启用 memory。

## 待决事项

以下事项在实现过程中需要最终确定：

- memory API 是否只放在 `wuwe::agent::memory`，还是部分 re-export 到 `wuwe`。
- 持久化 store 是否包含在 core library，还是作为可选组件构建。
- token counting 使用 provider-specific 方案，还是使用通用近似方案。
- 长期记忆写入前是否支持 review callback。
- summary 由用户函数生成，还是提供可选 LLM-backed summarizer。

第一版实现不应被这些事项阻塞，除非它们影响 ABI 或 public API 稳定性。

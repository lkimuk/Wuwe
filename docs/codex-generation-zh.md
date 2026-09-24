---
id: codex-generation-zh
title: Codex 生成、流式与工具调用接入
description: 将 OAuth 账户绑定到 Wuwe llm_client 和 Agent 工具循环。
---

# Codex 生成接入（第 4 步）

`codex_llm_client` 实现现有 `llm_client`，支持文本生成、流式文本、可见推理摘要、函数工具调用、
JSON Schema 输出和 usage。认证来自第 3 步的账户管理器；不会读取 API Key 环境变量，不能通过
普通 `OpenAI` 预设创建。账号登录与目录仍使用[OAuth 接口](codex-oauth-integration-zh.md)。

## 建立账户与模型绑定

```cpp
#include <wuwe/agent/llm/codex_llm_client.h>

// accounts 已注册 make_codex_oauth_refresher()，并持久化了登录账户。
// account_key 来自登录结果/账户列表，selected_model 来自该账户模型目录。
wuwe::codex_llm_client model(accounts, account_key, {
  .model = selected_model,
  .client_version = deployment_protocol_version,
});

wuwe::llm_request request;
request.messages = {
  { .role = "system", .content = "简洁、准确地回答。" },
  { .role = "user", .content = "解释这个接口的作用。" },
};
auto response = model.complete(request, stop_token);
if (!response.error_code) {
  // 展示 response.content。
}
```

manager 必须比 client 活得更久，销毁前结束调用。一个 client 固定绑定一个本地账户 key；
`request.model` 可覆盖默认模型，不会自动选择模型或改绑账号。`client_version` 与目录查询使用
相同的上游协议版本。可注入支持并发、同步流式回调和取消的 `shared_ptr<http_client>`。

`complete()` 和 `complete_stream()` 共用同一个增量解码器。Codex HTTP 请求始终使用
`POST https://chatgpt.com/backend-api/codex/responses`、`stream:true`、`store:false`；
普通 `complete()` 在本地聚合结果。令牌与 `chatgpt-account-id` 从同一账户快照获取。

## 流式事件与失败语义

```cpp
wuwe::llm_stream_callbacks callbacks;
callbacks.on_event = [&](const wuwe::llm_stream_event& event) {
  if (event.type == wuwe::llm_stream_event_type::content_delta) {
    // 追加 event.content_delta；直到 done 前均为暂时输出。
  }
};
callbacks.on_reasoning_delta = [&](std::string_view summary) {
  // 只展示服务端明确提供的可见摘要；可能没有摘要事件。
};
auto response = model.complete_stream(request, callbacks, stop_token);
```

- 文本、摘要和函数参数增量即时交付。不要根据 `tool_call_delta` 执行工具。
- 只有收到合法 `response.completed`、确认 HTTP 成功且账户代际未变化后，才交付
  `tool_call_done` 和最终 `done`。`[DONE]`、HTTP EOF 或已有部分文本均不能替代成功终态。
- 失败返回 error，不交付可执行的 `tool_calls` 或 `provider_state`。已显示的文本无法撤回，
  `response.content` 可能保留部分输出，界面应标记中断，不能视为完整回答。
- `response.failed`、`response.incomplete`、解析失败、参数损坏、重复工具调用 ID、超限和断流
  都会失败；不会将不完整工具请求交给 Agent。上游错误正文不会回传到错误消息。
- 不自动重试生成，包括 401/429/5xx；更不会在已有输出后重放请求。应用可在明确策略下重试新请求。
- 回调抛出的异常会先终止传输，再向调用方重抛，避免通过 libcurl 的 C 回调栈展开 C++ 异常。
- 成功通知是本次调用的完成点。通知之后的应用操作不能追溯撤销已经完成的响应。

账户问题保留 `wuwe.oauth` 错误码，例如 `reauthentication_required`、`account_changed`、存储失败。
生成协议、HTTP、传输、取消和超时使用 `llm_error_code`。流式响应超限为新增的
`response_limit_exceeded`；`metadata["http_status"]` 提供可获得的 HTTP 状态。

## 直接复用 Agent 工具循环

```cpp
#include <wuwe/agent/llm/llm_agent_runner.h>

// my_tool_provider 使用现有 tools()/invoke() 或反射工具接口。
wuwe::llm_agent_runner agent(model, my_tool_provider, 4);
auto answer = agent.complete(request);
```

模型适配器只描述工具调用，不执行本地操作。工具调用授权、执行、并发策略、审批暂停及轮次预算
继续由现有 Agent 管理。工具定义映射为 Responses `function`，历史工具调用使用 `call_id`，结果
映射为 `function_call_output`。同批多个函数调用分别保留 ID、名称、参数，不混合到文本输出。

`llm_tool_choice` 支持 auto、none、required 和 named；工具名称须符合上游函数命名约束
（1–64 个英文字母、数字、下划线或短横线）。历史中不能有孤立工具结果或尚未返回结果的调用。
工具 schema 默认 `strict:false`，保持 Wuwe 对可选属性的原有语义。

## 多轮状态与持久化

Codex 的无服务端存储模式需要回传加密推理项，并保留原生输出顺序和消息阶段
（`commentary` / `final_answer`）。公共类型新增可选的
`llm_provider_state { provider, data }`，分别位于 `llm_response` 和 `chat_message`：

```cpp
history.push_back({
  .role = "assistant",
  .content = response.content,
  .reasoning_content = response.reasoning_summary,
  .tool_calls = response.tool_calls,
  .provider_state = response.provider_state,
});
// 如有工具调用，再追加每个对应 call_id 的 role=tool 消息。
```

使用 `llm_agent_runner` 时，普通工具往返、模型续答和审批暂停/恢复会自动保留这份状态。
手工管理历史的调用方必须随原 assistant 消息保存并原样传回。不要展示、解释、修改或当作认证
令牌使用 `data`，也不要把它放进普通日志。它绑定供应商、本地账户、上游用户/workspace 和模型，
跨账号或跨模型重放会在请求前被拒绝。不支持该状态的供应商会明确返回 unsupported，而非默默丢弃。
状态与原消息的文本及工具调用必须匹配；修改这些内容却保留原状态会在请求前被拒绝。
工具参数仅改变 JSON 空白或键顺序不影响匹配。状态仅保存续接所需字段，不保存隐藏推理明文。

持久化 continuation 编解码向后兼容没有该字段的历史数据。协议状态与对应消息一起估算上下文
预算；不能单独截断含状态的消息，完整历史组仍可由上下文策略移除。这是保守的大小估算，不宣称
知道加密内容的真实 token 数。跨进程恢复使用稳定账户身份，不依赖只在内存中有效的登录代际。

可见摘要位于 `reasoning_summary`；加密推理只位于不透明协议状态中。普通语义记忆不是完整的
线协议历史存储，应用若自行保存多轮会话，应保存完整消息（含 provider_state）。

## 参数支持与限制

| 参数/能力 | 行为 |
| --- | --- |
| `model` | 请求覆盖 client 默认值，至少一处必填 |
| system/developer 消息、语言偏好 | 转为 `instructions`，语言控制是提示契约 |
| `json_schema_output` | 转为 `text.format`，实际支持仍取决于所选模型 |
| `reasoning_effort`（client options） | 可选 none/minimal/low/medium/high/xhigh；不猜测模型支持的档位 |
| `reasoning_summary`（client options） | 默认请求 auto 可见摘要，不解密或输出隐藏推理 |
| `parallel_tool_calls`（client options） | 默认 true；是否并行执行由应用工具层决定 |
| `max_output_tokens` | 后端不接受，显式返回 unsupported；不悄悄丢掉预算 |
| `temperature` | 公共字段为不可选 double；遗留默认值 0.2 在本适配器中视为未设置、不发送，其他值返回 unsupported |
| stop、seed、显式 cache 控制、通用 `thinking_mode` | 不支持，明确拒绝 |
| `response_format="json_object"` | 未声明支持；需要结构化输出时使用 JSON Schema |
| 图像/音频、内置联网工具、custom tool、WebSocket、服务端会话和压缩 | 本阶段不提供 |

`capabilities()` 显式声明当前支持范围。目录中出现模型、schema 请求合法或 OAuth 登录成功，均
不保证上游配额、具体模型特性或在线调用一定成功。

## 超时与资源限制

默认生成总超时 120 秒，覆盖生成 HTTP 调用；前置账户刷新由 refresher 自身超时约束。
`stream_timeouts` 可设置 total/connect/first_event/idle。首事件和空闲计时由有生命周期管理的
watchdog 通过 stop token 取消等待，不依赖后续字节到来才发现超时；注入的传输必须响应取消。
注释心跳不能无限延长首事件/空闲期限。超时结果附带可确定的 `timeout_phase`。

默认请求上限 8 MiB、流总上限 16 MiB、单 SSE 事件上限 2 MiB、输出项上限 1024，均可在
`codex_llm_options` 内有界配置。账户删除或重新登录后停止交付旧代际事件；已发送给上游的请求
无法通过本地删除追回。client 没有隐式会话缓存，可用同一实例进行独立并发调用。

## 验证依据

第 4 步核对官方 [Responses 请求结构](https://github.com/openai/codex/blob/main/codex-rs/codex-api/src/common.rs)、
[SSE 处理](https://github.com/openai/codex/blob/main/codex-rs/codex-api/src/sse/responses.rs) 和
[CCS Codex Responses 映射](https://github.com/farion1231/cc-switch/blob/main/src-tauri/src/proxy/providers/transform_responses.rs)。
本次官方网页读取返回 Forbidden/Cloudflare，没有将其视为已读文档依据。

测试使用合成凭据与协议响应，包括流分片、Unicode、终态快照、函数参数、加密状态往返、
真实 Agent 工具循环及审批恢复、取消/退出/重登、超时、错误和边界。底层 HTTP 测试使用本机服务。
线上验收脚本位于 `tools/codex-online-smoke.ps1`。它要求调用方通过环境变量提供已登录账户的
短期 access token、workspace/account ID 和模型 ID，不会读取或打印本地凭据。运行：

```powershell
$env:WUWE_CODEX_ACCESS_TOKEN = '<短期 access token>'
$env:WUWE_CODEX_ACCOUNT_ID = '<chatgpt-account-id/workspace>'
$env:WUWE_CODEX_MODEL = '<目录中的模型 slug>'
powershell -ExecutionPolicy Bypass -File tools/codex-online-smoke.ps1
```

脚本验证 HTTPS 模型目录、模型存在性、生成请求、SSE 文本增量和 `response.completed`。
本工作区没有真实账户凭据，因此这里只完成了端点可达性和未授权边界探测；接入方运行脚本后，
才可把真实账户结果记录为线上验收证据。

---
id: codex-oauth-integration-zh
title: Codex OAuth 接口方指南
description: 设备登录、账户模型目录、刷新和错误处理。
---

# Codex OAuth 接口方指南

第 3 步提供 `codex_oauth_client`：设备登录、定向重新登录、取消，以及登录账户的模型目录。
它依赖已有账户管理器和安全存储，供应商命名空间为 `codex_oauth`。第 4 步的生成、流式输出和
工具调用使用独立 [codex_llm_client](codex-generation-zh.md)；不要将这里的令牌填入普通 `OpenAI` 预设的 `api_key`。

## 初始化与生命周期

```cpp
#include <wuwe/agent/auth/codex_oauth.h>
#include <wuwe/agent/auth/oauth_credential_store.h>

wuwe::codex_oauth_options options {
  .client_id = deployment_client_id,
  .client_version = deployment_protocol_version,
};
auto refresher = wuwe::make_codex_oauth_refresher(options);
auto store = wuwe::make_system_oauth_credential_store(absolute_vault_path);
wuwe::oauth_account_manager accounts(std::move(store), { refresher });
wuwe::codex_oauth_client codex(accounts, options);
```

`client_id` 和 `client_version` 必须显式设置；版本影响服务端返回的目录，不能把 Wuwe 自身版本
当作上游协议版本。`originator` 默认 `codex_cli_rs`，是当前协议兼容请求头，不表示 Wuwe 是官方客户端。
调用方负责确认部署可使用的客户端身份。SDK 不复制 CCS/Codex 内置 client ID，也不导入其他应用凭据。

Windows 内置 vault 使用当前用户 DPAPI；目录须由应用预先创建并管理，传入绝对路径。
其他平台应注入实现安全持久化的 `oauth_credential_store`；无隐式明文或内存回退。
同一 vault 只建一个 manager，供多个服务对象共享。manager 必须比 client 活得更久，销毁前结束所有调用。

可向 client 和 refresher 注入同一个 `shared_ptr<http_client>`。它必须支持并发请求、有界超时与
取消，且不能记录凭据请求头、请求体或响应正文。默认 HTTP 客户端会校验 TLS；本适配器固定可信
HTTPS 地址并禁止重定向。构造函数不会发起请求。

## 登录与模型选择

1. 调用 `start_login(stop)`；成功返回 `challenge`。向用户展示其 `verification_uri` 和 `user_code`。
   `session_id` 是本地随机句柄，不是服务端设备 ID；SDK 不打开浏览器。
2. 应用按 `poll_after` 调用 `poll_login(session_id, stop)`。这是同步的单次轮询；请在工作线程执行。
   内部限制最小间隔，提前调用不会发送额外请求。同一会话并发调用只允许一个请求/交换在途。
3. `authorized` 才表示账户已持久化；保存返回的 `account->key`，普通配置和 UI 不保存令牌。
4. 调用 `list_models(account_key, stop)`；展示 `models[].id` 和可选 `display_name`。
   应用将选定的账户 key 和模型 ID 一起保存，并用这两个值构造 `codex_llm_client`。

```cpp
auto started = codex.start_login(stop);
if (started.error) {
  // 处理错误，不访问 challenge。
  return;
}
const auto challenge = *started.challenge;
// UI 展示 challenge.verification_uri / user_code；按 poll_after 安排轮询。
auto polled = codex.poll_login(challenge.session_id, stop);
if (polled.state == wuwe::oauth_login_state::authorized && polled.account) {
  const auto bound_account = polled.account->key;
  auto catalog = codex.list_models(bound_account, stop);
  if (!catalog.error) {
    // 展示 catalog.models；空列表也是有效结果。
  }
}
```

SDK 不隐式选择默认账户，也不切换到另一个账户重试。目录请求的令牌与 workspace 从同一个
账户快照取得；请求期间退出或重新登录会使旧结果返回 `account_changed`。成功列表按首次出现
的顺序去重；格式错误、超限或失败时列表始终为空。当前解析官方 `models[].slug` 格式，保留
完整目录，不按可见性过滤。目录中出现某模型不保证配额充足、账户一定有调用权或普通 API Key 可用。

## 状态与重试

| 返回状态/错误 | 应用处理 |
| --- | --- |
| `pending` | 按返回的 `poll_after` 继续；设备轮询的普通 403/404 是待确认 |
| `slow_down` | 使用增大的间隔；429 同时返回 `rate_limited`，尊重整数秒 `Retry-After` |
| `authorized` | 持久化完成，使用返回账户 key |
| `denied` / `expired` / `cancelled` | 当前会话结束；如用户仍需登录，重新调用 start |
| 轮询暂时性网络错误/5xx | 保持 pending，可按间隔再试；错误不包含上游正文 |
| 授权码交换失败 | 会话终止，重新登录；不盲目重放可能已被消费的授权码 |
| 交换成功但 `storage_failed` 等可恢复存储错误 | 未登录成功；会话有效期内再次 poll 只重试保存，不再次交换授权码 |
| `account_exists` | 相同用户与 workspace 已有本地账户；使用已有 key 或显式重登 |
| `account_changed` | 原账户已删除、重新登录，或定向重登身份不符；重新读取账户列表 |
| `reauthentication_required` | 向用户提供重新登录入口；目录 401 不会直接销毁 refresh token |
| `limit_exceeded` / `invalid_response` | 不展示部分结果；检查限制或上游协议变化 |

会话最多保留 15 分钟，服务端更短的期限会生效；默认最多 16 个，包含尚在发起的请求。
开始新会话时清理已结束/过期且无在途调用的会话，因此旧句柄随后可能返回 `invalid_argument`。
会话不落盘，应用重启后必须重新发起；账户凭据独立持久化。

`cancel_login(session_id)` 结束整个会话，并阻止晚到响应保存账户。若保存已经成功，取消不撤回
已完成的登录；需要退出时调用 `remove_account(key)`。预先取消的 poll 不发送请求；poll 执行期间
取消会结束正在处理的会话，避免丢弃已取得的授权码后又重复使用。并发旁观轮询无需等待网络。

## 定向重新登录与刷新

```cpp
auto reauth = codex.start_reauthentication(existing_account_key, stop);
// 后续同样展示 challenge、poll；成功保持原本地 account_id。
```

定向重登同时绑定原用户、workspace 和账户代际，不能登录为另一个用户，即使两者属于同一个
workspace。原账户中途删除或被其他登录替换时，旧会话不能覆盖它。邮箱只用于展示，不作为唯一身份。

`make_codex_oauth_refresher()` 与设备登录共享内部协议解析。返回 `expires_in` 时使用实际期限；
缺失时读取可信 token 响应中 access JWT 的 `exp`，无可信有效期则失败。令牌包含新 ID token 时
校验 issuer、audience、有效期和稳定身份；刷新不允许变更用户或 workspace。缺失 refresh/id token
时保留已有值。解析 JWT 声明依赖来自固定 TLS token endpoint 的响应，**不是任意外部 JWT 的
密码学签名验证服务**。企业/FedRAMP 特殊路由不在此次范围内。

manager 协调同一账户的并发刷新并持久化轮换。刷新成功后才收到取消时仍保存新令牌，再向调用方
报告取消，避免丢掉轮换结果。上游轮换与磁盘保存之间没有跨系统原子事务；网络中断或进程崩溃仍
可能需要重新登录，详见[账户与存储设计](oauth-integration-design.md)。

底层 `save_authorization(..., expected_revision)` 供可信适配器使用：0 表示仅新增；非零值来自
`account_snapshot()`，用于有条件替换。`access_token()` 同时返回对应账户元数据和代际，供服务层
绑定请求，UI 应只调用 `accounts()` / `account_snapshot()`。这些代际只在 manager 生命周期内有效。

## 验证范围与协议依据

离线测试使用合成响应和 JWT，覆盖设备流程、账户隔离、刷新、并发、取消、保存失败、目录与限制；
没有访问用户真实账号。真实 client ID 的准入、在线授权和实际目录尚待端到端验证，不能据离线测试
宣称与成熟 CCS 具有相同线上可靠性。

除[设计文档中的授权源码](oauth-integration-design.md)外，第 3 步核对了官方
[ModelsClient](https://github.com/openai/codex/blob/main/codex-rs/codex-api/src/endpoint/models.rs)、
[token_data](https://github.com/openai/codex/blob/main/codex-rs/login/src/token_data.rs)及 CCS 的
[Codex 目录服务](https://github.com/farion1231/cc-switch/blob/f8788719a19be6cdef39151b1c0607b4608fa10a/src-tauri/src/services/codex_oauth_models.rs)。
这些是兼容实现依据，不承诺上游未公开接口长期稳定。

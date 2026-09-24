---
id: oauth-integration-design
title: OAuth 账户接入设计与阶段交付
description: Codex 授权协议核实、公共 API、凭据存储和刷新管理边界。
---

# OAuth 账户接入：第 1–4 步

当前已实现协议调查、公共 API 契约、账户与凭据管理、公共 OAuth 刷新客户端，
以及 **Codex 设备授权、专用令牌刷新和账户模型目录**。第 3 步接入方式见
[Codex OAuth 接口方指南](codex-oauth-integration-zh.md)。
第 4 步已提供独立 [Codex 生成适配器](codex-generation-zh.md)，接入文本、SSE 和函数工具循环。
真实账户端到端验收留待第 5 步。不新增供应商预设，不修改
现有 `llm_client_config::api_key` 语义，不读取 Codex、CCS 或浏览器已有凭据。

## 协议核实（2026-09-24）

官方文档入口 `https://developers.openai.com/codex/auth/` 在本次环境返回 Forbidden，
未把该页面当作已读依据。实际核对以下源码：

- [OpenAI Codex 设备授权](https://github.com/openai/codex/blob/51d45620702cd3c825d3520ddc7d7e8052a16b87/codex-rs/login/src/device_code_auth.rs)
- [OpenAI Codex 凭据存储](https://github.com/openai/codex/blob/51d45620702cd3c825d3520ddc7d7e8052a16b87/codex-rs/login/src/auth/storage.rs)
- [CCS Codex OAuth 适配](https://github.com/farion1231/cc-switch/blob/f8788719a19be6cdef39151b1c0607b4608fa10a/src-tauri/src/proxy/providers/codex_oauth_auth.rs)

| 阶段 | 核实结果及 Wuwe 决策 |
| --- | --- |
| 发起设备授权 | POST `https://auth.openai.com/api/accounts/deviceauth/usercode`，包含 client ID；由适配器持有上游会话标识 |
| 用户确认 | 展示 `https://auth.openai.com/codex/device` 与用户码；SDK 不操作浏览器 |
| 轮询 | POST `/api/accounts/deviceauth/token`，保留服务端轮询间隔；官方客户端限制等待约 15 分钟 |
| 交换令牌 | 成功轮询得到 authorization code 和服务端返回的 PKCE verifier，再向 `/oauth/token` 交换；不能把设备码直接当作访问令牌 |
| 刷新 | CCS 使用公共客户端 refresh_token grant，令牌可能轮换；新 refresh token 持久化成功后才向服务适配器交付访问令牌 |
| 账户绑定 | 本地稳定账户 ID 与上游 workspace/account ID 分开，不以邮箱作为唯一键 |
| 模型与生成 | Codex 使用专用目录与 Responses/SSE 协议；已提供独立适配器，不能将 OAuth 令牌直接替换普通 OpenAI API Key |
| 安全存储 | 官方源码包含 keyring、文件、自动回退等选项；Wuwe 默认安全存储不做明文回退，内存存储必须显式选择 |

这些是开源实现依据，不是对第三方复用某个 client ID 的授权证明，也不承诺未公开端点
长期稳定。**代码不硬编码 Codex client ID；仅在应用显式调用登录、刷新或目录 API 时请求服务。**
`codex_oauth_options` 要求接入方提供其部署可使用的 client ID 和目录协议版本；不能以
“CCS 能用”代替部署方确认客户端身份和账户准入。离线实现与测试不要求用户提供真实凭据。
公共刷新客户端严格要求 `expires_in`；缺失该字段或使用不同响应结构的服务应在其适配器
中依据可靠协议确定有效期，不能在公共层猜一个有效期。

## 模块与公共头文件

| 模块 | 入口 | 职责 |
| --- | --- | --- |
| 账户管理 | `wuwe/agent/auth/oauth_account.h` | 账户身份、状态、刷新协调、错误、存储与刷新扩展接口 |
| 设备授权契约 | `wuwe/agent/auth/oauth_authorization.h` | 发起、单次轮询、取消的抽象接口 |
| 凭据存储 | `wuwe/agent/auth/oauth_credential_store.h` | 显式内存存储、Windows DPAPI 加密存储 |
| 公共刷新协议 | `wuwe/agent/auth/oauth_refresh_client.h` | HTTPS、表单编码的公共客户端 refresh_token grant |
| Codex 适配 | `wuwe/agent/auth/codex_oauth.h` | 具体设备登录、指定账户重登、账户模型目录与专用 refresher 工厂 |
| Codex 生成 | `wuwe/agent/llm/codex_llm_client.h` | 账户绑定、请求映射、流式解码、工具协议与多轮状态 |

上述头文件也由 `wuwe/wuwe.h` 导出。授权适配器依赖账户管理器，账户管理器依赖
存储接口与供应商刷新接口。账户管理器不依赖 LLM 工厂、模型选择界面或具体 Codex 协议。

设备授权接口只建模 device authorization，不强制将未来浏览器回调等流程塞进同一接口。
UI 侧只接触 session handle、验证网址、用户码、轮询间隔及结果状态。上游 device ID、
PKCE verifier 和令牌留在授权适配器内部。授权成功状态必须在账户持久化成功后返回。

## 账户与刷新管理

`oauth_account_key { provider, account_id }` 精确绑定账户。`accounts()` 只返回
非敏感账户元数据；没有隐式默认账户、自动故障切换或后台刷新线程。

`save_authorization()` 是供可信授权适配器使用的低层入口：适配器需先确认账户身份、
有效期和令牌来源。它不是“传入任意 JWT 就已经验证登录”的接口。

```cpp
#include <wuwe/agent/auth/oauth_credential_store.h>
#include <wuwe/agent/auth/oauth_refresh_client.h>

// vault_path 是应用自建私有本地目录中的绝对文件路径。
// refresh_config 中的 provider_id、HTTPS token URL、client ID 来自已确认的接入配置。
auto store = wuwe::make_system_oauth_credential_store(vault_path);
auto refresher = std::make_shared<wuwe::oauth_refresh_client>(refresh_config);
wuwe::oauth_account_manager accounts(std::move(store), { refresher });

for (const auto& account : accounts.accounts()) {
  // 显示 account.display_name，保存 account.key；不包含密钥。
}
// 服务适配器内部：accounts.access_token(bound_account_key, stop_token)。
// 普通应用配置不应保存这个访问令牌，也不应把它回传给界面。
```

同一个 vault 在进程中只有一个 manager，业务客户端共享其引用；销毁前需结束所有调用。
不同账户可并行访问网络；同一账户只允许一个刷新请求在途。等待者可以独立取消；发起者
的取消可能终止本次共享刷新，后续调用可重试。传输或刷新适配器必须提供有界超时并支持取消。

| 情况 | 明确行为 |
| --- | --- |
| 令牌有效且距离到期超过提前量 | 返回已保存的访问令牌，默认提前量 60 秒 |
| 令牌临近到期 | 使用该账户绑定供应商的 refresher，不选其他账户 |
| 刷新未返回 refresh/id token | 保留原值；显式空值或非法响应不当作有效轮换 |
| 刷新期间重新登录 | 替换账户代际；旧响应返回 `account_changed`，不能覆盖新登录 |
| 刷新期间成功移除账户 | 旧响应不能重建已移除账户；已交付给外部的令牌无法本地撤回 |
| 刷新成功时收到取消 | 仍先持久化新令牌，调用方收到 `cancelled` |
| 刷新轮换后写入失败 | 不向调用方交付新令牌；当前 manager 阻止重复使用旧 refresh token，状态显示需重登 |
| 明确 invalid_grant 等失效 | 持久化需重登状态；后续不持续刷新 |
| 网络错误、429、5xx、普通 401 | 返回刷新失败，保留账户；不会凭单个 HTTP 状态判定 refresh token 永久失效 |
| 移除/登录保存失败 | 保留原内存和磁盘状态，返回存储错误 |

上游令牌轮换与本地磁盘写入不能跨系统构成原子事务。写入失败或进程在二者之间崩溃时，
可能需要重新登录；不能承诺一定恢复旧 refresh token。接口保守失败，不隐藏这种情况。

`remove_account()` 只移除本地账户及其凭据，**不等同于上游 revoke**。本阶段不提供
“退出所有服务端会话”承诺。账户不存在、需重登、取消、代际改变、存储问题均有独立
`oauth_error`。构造失败抛 `std::system_error`，无效构造参数抛 `std::invalid_argument`；
正常操作返回错误码，扩展实现的运行时异常会收敛成存储/刷新失败，不回传上游错误正文。

## 凭据存储约束

- Windows：使用当前用户 DPAPI 加密整个快照；密文及临时文件带当前用户/SYSTEM 私有
  ACL；先写密文、flush，再在同目录原子替换。独占 sidecar 文件锁覆盖存储对象全生命周期，
  第二个 owner 返回 `storage_in_use`，避免多进程同时轮换同一批令牌。
- vault 目录由应用预先创建并管理，必须使用本地受控目录；不支持不可信共享目录、网络盘
  或绕过锁文件的其他写入者。不要让两个不同路径别名指向同一 vault。
- 存储上限：256 个账户、8 MiB 加密快照；管理器默认每个令牌字段不超过 256 KiB。
  加密文件格式带版本，不能解密或格式损坏时拒绝加载，不静默覆盖成空账户。
- 非 Windows：内置系统存储明确返回 `storage_unavailable`。可注入平台 keychain 后端；
  本次没有宣称实现 macOS Keychain 或 Linux Secret Service。
- 内存存储仅在显式调用 `make_memory_oauth_credential_store()` 时启用，适合临时会话和测试。
- 自定义存储须保证独占所有权、成功保存完整快照、失败保留旧快照；不允许回调 manager。
- DPAPI 防止磁盘明文泄露，但不防御已控制同一用户进程的攻击者。当前 C++ 字符串和 JSON
  解析有内存副本，不宣称所有秘密都常驻锁页内存或被可靠擦除；临时 DPAPI 解密缓冲会清零。

公共刷新客户端使用 HTTPS、禁用重定向、不自动重试，按配置限制响应体，使用显式 timeout。
其输出只面向服务适配边界，不向 UI 暴露令牌。HTTP 传输由应用注入时，必须禁止日志、跟踪或
监控记录 token 请求/响应正文；通用 HTTP 拦截器不因本模块存在就自动完成敏感字段脱敏。

## 验收与后续

本阶段测试覆盖：账户隔离、保存/删除失败、轮换合并、刷新异常、明确失效、并发刷新合并、
等待者取消、刷新期间退出/重新登录、轮换后取消、写入失败阻止旧令牌重用、HTTP 表单与错误
解析、响应限制，以及真实 Windows DPAPI 写入/重启读取/锁冲突/损坏检测。

第 3 步增加设备登录/交换、403/404 待确认、拒绝/过期/降频、重复账户、定向重登、
并发轮询与取消、授权后保存重试、JWT 有效期与身份约束、目录头部与完整性、
目录请求期间账户删除等离线测试。公共头文件独立编译与安装包消费测试覆盖新入口。

第 4 步已接入生成、流式事件、工具调用及审批恢复状态。第 5 步提供
`tools/codex-online-smoke.ps1` 作为真实账户验收入口；当前开发环境没有真实凭据，因此未把线上成功
冒充为已验证。实际部署需使用短期令牌完成目录、模型选择、SSE 完成事件和错误边界验收。

---
id: llm-integration-update-zh
title: 模型发现与供应商更新接入指南
description: 面向 Wuwe C++ 接口调用方的配置、模型发现、错误处理及升级说明。
---

# 模型发现与供应商更新接入指南

本指南面向使用 Wuwe C++ SDK 的应用、服务及配置界面开发者。此次更新提供主动查询供应商模型列表的 API，并新增 7 个供应商预设。它不是新增的 HTTP 服务端接口；如果产品通过 HTTP 对外提供模型选择功能，应由应用层封装该 C++ API。

## 1. 更新范围与升级方式

更新包含以下提交；接入时请使用包含三者的版本：

| 提交 | 内容 |
| --- | --- |
| `c5dc479` | 新增 7 个供应商预设及其协议兼容策略 |
| `a71c9e7` | 模型发现 API、分页与限制、统一地址拼接 |
| `4c010bc` | 智谱模型目录兼容及 CCS 对照验证 |

重新构建、安装 Wuwe，并重新编译链接调用方。公共类型有新增字段，不应混用旧二进制库与新头文件。仍使用 C++20 和现有 `wuwe::wuwe` CMake 目标，无需引入额外 SDK。

已有聊天调用不需要迁移到新接口。可见行为变化是：生成、流式生成和模型发现统一处理地址末尾的斜杠及重复版本前缀。例如 OpenAI 兼容地址 `https://gateway.example/v1/` 不再拼出 `/v1/v1/chat/completions`。使用自定义代理路径的调用方应核对最终请求地址。

## 2. 最小接入示例

以下函数接收应用配置中的密钥，查询列表，再使用调用方选定的 ID 创建聊天客户端。实际界面中，查询列表和选择模型通常分为两次操作。

```cpp
#include <iostream>
#include <string>
#include <utility>
#include <wuwe/wuwe.h>

void query_and_call(std::string api_key, std::string selected_model_id) {
  wuwe::llm_client_config config {
    .api_key = std::move(api_key),
    .load_api_key_from_environment = false,
    .timeout = 10'000,
  };

  const auto result = wuwe::list_llm_models("Kimi", config);
  if (result.error_code) {
    std::cerr << result.error_code.message()
              << " (HTTP " << result.http_status << ")\n";
    return;
  }
  for (const auto& model : result.models) {
    std::cout << model.display_name.value_or(model.id)
              << " [" << model.id << "]\n";
  }

  if (selected_model_id.empty()) {
    return; // 等待用户选择，或通过手动输入提供模型 ID。
  }
  config.model = std::move(selected_model_id);
  auto client = wuwe::make_llm_client("Kimi", config);
  wuwe::llm_request request;
  request.messages.push_back({ .role = "user", .content = "你好" });
  const auto response = client->complete(request);
  if (response.error_code) {
    std::cerr << response.error_code.message() << '\n';
    return;
  }
  std::cout << response.content << '\n';
}
```

只查询列表时包含 `<wuwe/agent/llm/llm_model_discovery.h>` 即可，不要求提供 `config.model`。供应商 ID 区分大小写；应使用注册表 ID，不能传界面显示名称。自定义兼容服务使用 `OpenAICompatible` 并提供 `base_url`。仅向工厂注册自定义客户端，不会让该 ID 自动进入模型发现注册表。

## 3. API 与返回值

两个重载均为同步调用：

```cpp
list_llm_models(provider_id, config, options, stop_token);
list_llm_models(provider_id, config, http_client, options, stop_token);
```

`options` 与 `stop_token` 可省略；第二个重载借用 `http_client&`，适合复用应用传输层或注入测试实现，不转移所有权。

| 字段 | 调用方约定 |
| --- | --- |
| `result.error_code` | 先判断此字段；空错误码表示成功 |
| `result.models` | 所有页合并、按 ID 去重，保留首次出现的顺序和元数据；失败时始终为空 |
| `model.id` | 用于请求的 ID，不要替换成显示名称；Gemini 的 `models/` 前缀已移除 |
| `model.display_name` | 可选显示名称，缺失时展示 ID |
| `result.http_status` | 最后一次响应状态；未收到响应时可能为 0 |
| `result.transport_error` | 底层传输错误，用于诊断 |

成功但列表为空，与接口不支持查询是不同结果。列表可能包含嵌入等非聊天模型，也不证明当前账户有调用权限。不要根据目录自行推断工具调用能力、上下文长度或价格。

默认自动处理支持的分页，限制为 100 页、10,000 个唯一模型、每页 8 MiB 响应。`config.timeout` 默认 30,000 毫秒，必须为正，是整个发现操作的超时预算。可通过 `llm_model_list_options` 调整限制，所有限制必须为正。发现不会使用聊天的自动重试配置。

## 4. 新增供应商配置

显式 `api_key` 优先于环境变量。下面 7 个新预设只读取表中的专用变量，不回退到 OpenAI 或 OpenRouter 密钥。`load_api_key_from_environment = false` 可禁用环境变量加载。

| ID | 默认 base URL | 密钥环境变量，按优先顺序 |
| --- | --- | --- |
| `Kimi` | `https://api.moonshot.cn` | `MOONSHOT_API_KEY`、`KIMI_API_KEY` |
| `MiniMax` | `https://api.minimaxi.com` | `MINIMAX_API_KEY` |
| `SiliconFlow` | `https://api.siliconflow.cn` | `SILICONFLOW_API_KEY` |
| `Doubao` | `https://ark.cn-beijing.volces.com/api/v3` | `ARK_API_KEY`、`DOUBAO_API_KEY` |
| `Nvidia` | `https://integrate.api.nvidia.com` | `NVIDIA_API_KEY` |
| `StepFun` | `https://api.stepfun.com` | `STEPFUN_API_KEY` |
| `MiMo` | `https://api.xiaomimimo.com` | `MIMO_API_KEY` |

默认是普通 API 平台地址。国际区域、Coding Plan、Token Plan 的地址和密钥需要显式配置，SDK 不自动切换。供应商预设具有模型发现接入路径，不代表所有上游均开放该路径。

生成接口的关键差异：

- **Kimi、MiMo：** 支持通过 `thinking_mode` 发送显式思考开关，默认不发送开关。
- **MiMo：** `max_output_tokens` 映射为 `max_completion_tokens`；工具调用请保持 `tool_choice` 未设置，显式工具选择控制会被拒绝。
- **MiniMax：** 保留 `content` 中原生 `<think>...</think>` 内容；不声明独立推理流、显式工具选择、JSON 输出或 stop 控制。
- **SiliconFlow、Nvidia：** 保留完整的供应商限定模型 ID，例如 `vendor/model` 或 `Pro/...`。
- **Doubao：** 按部署要求传入模型 ID 或 `ep-...` 推理接入点 ID；模型列表不是管理后台的部署清单。

能力仍可能因模型、套餐或账户而缩小。使用注册表的能力声明进行基础校验，同时处理生成请求返回的错误。

## 5. 公开目录和本地服务

不带密钥查询公开目录，使用现有配置即可：

```cpp
wuwe::llm_client_config public_config {
  .require_api_key = false,
  .load_api_key_from_environment = false,
  .timeout = 10'000,
};
const auto catalog = wuwe::list_llm_models("OpenRouter", public_config);
```

该配置不发送密钥，最终是否允许访问仍由服务器决定。`require_api_key = false` 只关闭本地缺少密钥检查，不会绕过服务器鉴权；已有非空密钥仍会发送。不要将匿名目录配置直接当作已鉴权的聊天配置。

本地 Ollama 默认不要求密钥：

```cpp
const auto installed = wuwe::list_llm_models(
  "Ollama", { .load_api_key_from_environment = false });
```

默认查询 `http://localhost:11434/api/tags`。部署在其他地址时配置 `base_url`；代理要求鉴权时提供密钥或相应传输实现。

## 6. 自定义网关与目录路径

```cpp
wuwe::llm_client_config gateway_config {
  .base_url = "https://gateway.example",
  .api_key = "replace-with-configured-key",
  .load_api_key_from_environment = false,
};
const auto catalog = wuwe::list_llm_models("OpenAICompatible", gateway_config, {
  .format = wuwe::llm_model_list_format::openai,
  .models_path = "/catalog/models",
  .max_pages = 20,
  .max_models = 2'000,
  .max_response_bytes = 2 * 1024 * 1024,
});
```

`models_path` 直接追加到去掉尾斜杠的 `base_url` 后，必须以单个 `/` 开头，不接受绝对 URL、查询串或片段。例如 base 已包含 `/v1` 时，追加 `/models`，不要再追加 `/v1/models`。

`format` 同时决定解析方式和鉴权头：OpenAI/Ollama 使用 Bearer，Anthropic 使用 `x-api-key`，Gemini 使用 `x-goog-api-key`。不要仅为调整 JSON 解析而忽略其鉴权影响。自定义请求头等特殊需求通过注入的 HTTP 传输实现，当前 options 没有任意请求头字段。

兼容格式支持 `data[].id`，以及缺少 `data` 时的智谱 `models[].slug`。不会自动改用备用域名、剥离套餐路径、探测其他接口或跟随重定向。目录与聊天根路径不同，可复制配置后仅为本次发现调整 base 和 path。

## 7. 错误处理与交互建议

通过 `wuwe::agent::llm_error_code` 比较错误，不依赖错误消息文本：

```cpp
using wuwe::agent::llm_error_code;
// result 为本次 list_llm_models() 返回值。
if (result.error_code == llm_error_code::unsupported_capability) {
  // 提示检查地址，或手动填写模型 ID；不要清除已有选择。
}
```

| 错误 | 常见原因与处理 |
| --- | --- |
| `missing_api_key` | 本地要求密钥但未配置；补齐配置，公开目录可显式匿名查询 |
| `invalid_request` | 未知供应商、非法地址/路径、非正数超时或限制等；修正配置 |
| `authentication_failed` | HTTP 401/403；检查密钥、区域和账户权限 |
| `unsupported_capability` | HTTP 404/405/501；可能路径错误或目录接口未开放，保留手动输入 |
| `rate_limited` | HTTP 429；由应用决定退避后重试，避免连续自动刷新 |
| `timeout` / `transport_error` | 超时或网络错误；保留已有列表，允许重试 |
| `invalid_response` / `api_error` | 响应格式不合法、分页游标异常或上游错误对象 |
| `model_list_limit_exceeded` | 超出配置限制；按实际目录规模调整，失败不会返回部分列表 |
| `cancelled` | 查询已取消，通常无需展示为故障 |
| `http_error` | 其他非成功 HTTP 响应，包括未跟随的重定向 |

建议由“刷新模型”操作触发，在工作线程执行同步 API，避免阻塞 UI。应用负责缓存和刷新策略；缓存应区分供应商、地址及账户配置，不要混用不同账户的结果。配置变更后忽略旧查询返回值，查询失败不清除已有有效选择，并始终提供手动模型输入。

取消时，将 `std::stop_source` 的 token 作为最后一个参数传入，在另一线程或事件处理器调用 `request_stop()`。取消延迟取决于传输层对回调及 stop token 的支持。自定义 HTTP 客户端需实现相应行为，其异常按底层接口约定向外传播；共享传输的线程安全由调用方负责。

## 8. 接入验收

1. 使用目标版本重新编译链接，确认新头文件和库匹配。
2. 验证查询成功、合法空列表、密钥错误、接口不支持以及取消场景。
3. 验证自定义 base URL 的实际请求路径；不要将整个聊天 URL 当作 base URL。
4. 确认保存和调用使用模型 ID，显示名称仅用于界面。
5. 用目标账户及实际模型验证生成、流式输出和业务需要的工具调用。

本次更新已通过严格构建、相关回归、公共头文件独立编译及安装包消费验证；模型发现测试还覆盖本地 cpr/httplib 链路。CCS 源码对照用于验证兼容契约，不等同于所有供应商线上实测。

进一步参考：[供应商与完整协议说明](llm-providers.md)、[CCS 对照记录](llm-ccs-validation.md)、[可运行的模型查询示例](../examples/src/llm_models_example.cpp)。

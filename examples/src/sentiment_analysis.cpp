#include <windows.h>
#include <wuwe/wuwe.h>

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  wuwe::llm_config config {
    .base_url = "https://openrouter.ai/api",
    .model = "openai/gpt-oss-120b:free",
  };

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenRouter", config);

  auto extract = [](const std::string& prompt) {
    return "从以下用户评论中，提取所有提到产品功能、性能或使用体验的关键短语。只输出列表，不要额外"
           "解释。\n\n" +
           prompt;
  };
  auto classify = [](const wuwe::llm_response& response) {
    return "对以下每个功能点标记情感：正面（+）、负面（-）、中性（=）。只输出带标记的列表。\n\n" +
           response.content;
  };
  auto summarize = [](const wuwe::llm_response& response) {
    return "根据以下带情感标记的功能点，生成一句简短的产品总结（不超过20字）。只输出总结句。\n\n" +
           response.content;
  };

  auto chain = client | extract | classify | summarize;
  auto response = chain.invoke("我买了这个无线鼠标，连接很快，但电池续航太差了，基本上两天就要充一"
                               "次电。手感还不错，就是有点重。");
  wuwe::print("{}", response.content);
}

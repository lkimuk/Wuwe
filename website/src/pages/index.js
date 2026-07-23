import clsx from 'clsx';
import Link from '@docusaurus/Link';
import Layout from '@theme/Layout';
import CodeBlock from '@theme/CodeBlock';
import useBaseUrl from '@docusaurus/useBaseUrl';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import styles from './index.module.css';

const content = {
  en: {
    pageTitle: 'Native C++20 agent systems',
    description: 'Wuwe is a native C++20 framework for building composable, stateful, and auditable agent systems.',
    heroEyebrow: 'Native agent systems · C++20',
    heroTitle: 'Build agents that belong in your runtime.',
    heroBody: 'Wuwe brings model access, typed tools, reasoning, planning, memory, retrieval, MCP, and controlled execution into one composable C++ framework.',
    primaryAction: 'Read the documentation',
    secondaryAction: 'View on GitHub',
    stats: [
      ['Language', 'C++20'],
      ['Release', '0.1.0'],
      ['Builds', 'Windows · Linux'],
      ['Model', 'Composable · Auditable'],
    ],
    systemsEyebrow: 'One runtime, explicit boundaries',
    systemsTitle: 'Compose the parts you need. Keep ownership of the system.',
    systemsBody: 'Each subsystem has a focused contract and can be used independently. Higher-level reasoning and planning APIs compose them without hiding state, policy, or execution traces.',
    systems: [
      ['01', 'Model access', 'OpenAI-compatible APIs, Anthropic, Gemini, and Ollama'],
      ['02', 'Reasoning and planning', 'ReAct, reflection, plan execution, budgets, and approvals'],
      ['03', 'Tools and protocols', 'Typed dispatch, MCP servers and clients, HTTP and stdio adapters'],
      ['04', 'State and knowledge', 'Memory, embeddings, hybrid retrieval, citations, and Qdrant'],
      ['05', 'Execution and evidence', 'Capability checks, policy hooks, audit events, and traces'],
    ],
    modulesEyebrow: 'Composable modules',
    modulesTitle: 'Use one capability or assemble a complete runtime.',
    modulesBody: 'Sixteen focused modules share common contracts for events, state, policy, and execution. Adopt them independently, then compose only what the product needs.',
    moduleGroups: [
      ['Models and orchestration', [
        ['LLM providers', 'Unified clients for OpenAI-compatible APIs, Anthropic, Gemini, Ollama, and OpenRouter.', '/docs/llm-providers/'],
        ['Tools', 'Typed definitions, dispatch, structured results, and model-facing tool calls.', '/docs/llm-tools/'],
        ['Reasoning', 'Observable reasoning loops with budgets, tool use, and execution traces.', '/docs/reasoning/'],
        ['Reflection', 'Critique and iterative improvement backed by pluggable reflection stores.', '/docs/reflection/'],
        ['Planning', 'Plan generation, persistence, reflection, and controlled step execution.', '/docs/planning/'],
        ['Orchestration', 'Flow primitives for composing runtime operations without hiding control.', '/docs/orchestration/'],
      ]],
      ['State and knowledge', [
        ['Memory', 'Policy-driven memory with in-memory, file, SQLite, and Qdrant backends.', '/docs/memory-management/'],
        ['Knowledge / RAG', 'Loading, splitting, indexing, retrieval, reranking, grounding, and citations.', '/docs/knowledge-retrieval/'],
      ]],
      ['Protocols and transport', [
        ['MCP', 'Clients, servers, gateways, host runtime, and HTTP, stdio, or process transports.', '/docs/mcp/'],
        ['Networking', 'Selectable HTTP backends, streaming event parsing, and typed transport errors.', '/docs/http-backends/'],
      ]],
      ['Governance', [
        ['Capability policy', 'Explicit capability declarations and policy checks at runtime boundaries.', '/docs/security-governance/'],
        ['Approvals', 'Approval service contracts for actions that require host authorization.', '/docs/security-governance/'],
        ['Audit', 'Structured audit events with configurable sinks and host-owned retention.', '/docs/security-governance/'],
        ['Observability', 'Common event sinks, module observers, traces, and telemetry adapters.', '/docs/observability/'],
      ]],
      ['Execution', [
        ['Controlled execution', 'Execution registry, policies, path checks, and controlled process tools.', '/docs/execution-runtime/'],
        ['Sandbox', 'Backend enforcement contracts and explicit platform capability reporting.', '/docs/security-governance/'],
      ]],
    ],
    codeEyebrow: 'A small surface to start',
    codeTitle: 'From provider selection to a response in native code.',
    codeBody: 'The same provider abstraction extends to streaming, tool calls, reasoning strategies, and host-owned state.',
    codeAction: 'Get started',
    useCasesEyebrow: 'Where it fits',
    useCasesTitle: 'Built for products where native integration matters.',
    useCasesBody: 'Wuwe does not prescribe one application shape. It provides the runtime pieces and lets the host own the experience.',
    useCases: [
      ['Desktop applications', 'Add contextual assistants, document workflows, or operator tools without moving core state out of process.'],
      ['Backend services', 'Run tool-using and stateful agent workflows behind existing service boundaries and observability.'],
      ['Developer tooling', 'Compose planning, MCP, retrieval, and controlled execution for automation and engineering workflows.'],
      ['Private deployments', 'Select local providers, persistence, and external services according to the product deployment model.'],
    ],
    closingEyebrow: 'Documentation',
    closingTitle: 'Start with one module. Grow into an agent runtime.',
    closingBody: 'The documentation follows the same path: build first, then compose reasoning, state, protocols, and controlled execution.',
    closingAction: 'Start building',
  },
  'zh-Hans': {
    pageTitle: '原生 C++20 Agent 系统',
    description: 'Wuwe 是用于构建可组合、有状态、可审计 Agent 系统的原生 C++20 框架。',
    heroEyebrow: '原生 Agent 系统 · C++20',
    heroTitle: '构建真正属于你运行时的 Agent。',
    heroBody: 'Wuwe 将模型访问、类型化工具、推理、规划、记忆、检索、MCP 与受控执行组合进一套原生 C++ 框架。',
    primaryAction: '阅读文档',
    secondaryAction: '在 GitHub 查看',
    stats: [
      ['语言', 'C++20'],
      ['版本', '0.1.0'],
      ['构建平台', 'Windows · Linux'],
      ['模型', '可组合 · 可审计'],
    ],
    systemsEyebrow: '一个运行时，明确的边界',
    systemsTitle: '组合需要的部分，继续拥有整个系统。',
    systemsBody: '每个子系统都有聚焦的契约，可以独立使用。上层推理与规划 API 负责组合它们，但不会隐藏状态、策略或执行轨迹。',
    systems: [
      ['01', '模型访问', 'OpenAI 兼容接口，以及 Anthropic、Gemini 与 Ollama'],
      ['02', '推理与规划', 'ReAct、反思、计划执行、预算与审批'],
      ['03', '工具与协议', '类型化分发、MCP 服务端与客户端、HTTP 和 stdio 适配器'],
      ['04', '状态与知识', '记忆、Embedding、混合检索、引用与 Qdrant'],
      ['05', '执行与证据', '能力检查、策略钩子、审计事件与轨迹'],
    ],
    modulesEyebrow: '可组合模块',
    modulesTitle: '使用一个能力，或组合出完整运行时。',
    modulesBody: '十六个聚焦模块共享事件、状态、策略与执行契约。可以独立采用，再只按产品需要进行组合。',
    moduleGroups: [
      ['模型与编排', [
        ['LLM Provider', '统一支持 OpenAI 兼容接口、Anthropic、Gemini、Ollama 与 OpenRouter。', '/docs/llm-providers/'],
        ['工具', '类型化定义、分发、结构化结果以及面向模型的工具调用。', '/docs/llm-tools/'],
        ['推理', '具有预算、工具使用和执行轨迹的可观察推理循环。', '/docs/reasoning/'],
        ['反思', '基于可替换存储的批评、评估与迭代改进。', '/docs/reflection/'],
        ['规划', '计划生成、持久化、反思与受控步骤执行。', '/docs/planning/'],
        ['编排', '在不隐藏控制权的前提下组合运行时操作的 Flow 原语。', '/docs/orchestration/'],
      ]],
      ['状态与知识', [
        ['记忆', '策略驱动的记忆能力，支持内存、文件、SQLite 与 Qdrant 后端。', '/docs/memory-management/'],
        ['知识 / RAG', '加载、切分、索引、检索、重排、Grounding 与引用。', '/docs/knowledge-retrieval/'],
      ]],
      ['协议与传输', [
        ['MCP', '客户端、服务端、Gateway、宿主运行时，以及 HTTP、stdio 和进程传输。', '/docs/mcp/'],
        ['网络', '可选择的 HTTP 后端、流式事件解析和类型化传输错误。', '/docs/http-backends/'],
      ]],
      ['治理', [
        ['能力策略', '在运行时边界显式声明能力并执行策略检查。', '/docs/security-governance/'],
        ['审批', '为需要宿主授权的操作提供审批服务契约。', '/docs/security-governance/'],
        ['审计', '结构化审计事件、可配置 Sink 与宿主持有的留存策略。', '/docs/security-governance/'],
        ['可观测性', '公共事件 Sink、模块 Observer、轨迹与遥测适配器。', '/docs/observability/'],
      ]],
      ['执行', [
        ['受控执行', '执行注册表、策略、路径检查与受控进程工具。', '/docs/execution-runtime/'],
        ['沙箱', '后端执行契约与明确的平台能力报告。', '/docs/security-governance/'],
      ]],
    ],
    codeEyebrow: '从一个小接口开始',
    codeTitle: '从 Provider 选择到原生代码中的一次响应。',
    codeBody: '同一套 Provider 抽象可以继续扩展到流式输出、工具调用、推理策略和宿主持有的状态。',
    codeAction: '开始使用',
    useCasesEyebrow: '适用场景',
    useCasesTitle: '为重视原生集成的产品而构建。',
    useCasesBody: 'Wuwe 不规定唯一的应用形态，而是提供运行时组件，让宿主继续拥有最终体验。',
    useCases: [
      ['桌面应用', '加入上下文助手、文档工作流或操作工具，同时让核心状态留在进程内。'],
      ['后端服务', '在既有服务边界和可观测体系内运行工具调用与有状态 Agent 工作流。'],
      ['开发者工具', '组合规划、MCP、检索和受控执行，服务自动化与工程工作流。'],
      ['私有部署', '根据产品部署模型选择本地 Provider、持久化方式和外部服务。'],
    ],
    closingEyebrow: '文档',
    closingTitle: '从一个模块开始，逐步构建 Agent 运行时。',
    closingBody: '文档遵循同一条路径：先完成构建，再组合推理、状态、协议与受控执行。',
    closingAction: '开始构建',
  },
};

const example = `#include <iostream>
#include <wuwe/wuwe.h>

int main() {
  wuwe::llm_config config{.model = "gpt-4.1-mini"};

  wuwe::llm_client_factory factory;
  auto client = factory.create_shared("OpenAI", config);
  const auto response = client->complete("Explain RAII briefly.");

  if (!response) return 1;
  std::cout << response.content << '\\n';
}`;

function Home() {
  const {i18n} = useDocusaurusContext();
  const text = content[i18n.currentLocale] ?? content.en;
  const markLight = useBaseUrl('/wuwe-mark-light-transparent.png');
  const markDark = useBaseUrl('/wuwe-mark-dark-transparent.png');

  return (
    <Layout title={text.pageTitle} description={text.description}>
      <main>
        <header className={styles.hero}>
          <div className={styles.heroGrid}>
            <div className={styles.heroCopy}>
              <p className={styles.eyebrow}>{text.heroEyebrow}</p>
              <h1>{text.heroTitle}</h1>
              <p className={styles.heroBody}>{text.heroBody}</p>
              <div className={styles.actions}>
                <Link className="button button--primary" to="/docs/">
                  {text.primaryAction}
                </Link>
                <a className="button button--secondary" href="https://github.com/lkimuk/Wuwe">
                  {text.secondaryAction}
                </a>
              </div>
            </div>

            <div className={styles.heroMark} aria-hidden="true">
              <span>WUWE / 0.1.0</span>
              <img className={styles.markLight} src={markLight} alt="" />
              <img className={styles.markDark} src={markDark} alt="" />
            </div>
          </div>

          <dl className={styles.stats}>
            {text.stats.map(([label, value]) => (
              <div key={label}>
                <dt>{label}</dt>
                <dd>{value}</dd>
              </div>
            ))}
          </dl>
        </header>

        <section className={styles.section}>
          <div className={styles.sectionIntro}>
            <p className={styles.eyebrow}>{text.systemsEyebrow}</p>
            <h2>{text.systemsTitle}</h2>
            <p>{text.systemsBody}</p>
          </div>
          <ol className={styles.systemList}>
            {text.systems.map(([number, title, detail]) => (
              <li key={number}>
                <span>{number}</span>
                <div>
                  <h3>{title}</h3>
                  <p>{detail}</p>
                </div>
              </li>
            ))}
          </ol>
        </section>

        <section className={styles.moduleSection}>
          <div className={styles.moduleIntro}>
            <div>
              <p className={styles.eyebrow}>{text.modulesEyebrow}</p>
              <h2>{text.modulesTitle}</h2>
            </div>
            <p>{text.modulesBody}</p>
          </div>
          <div className={styles.moduleCatalog}>
            {text.moduleGroups.map(([group, modules]) => (
              <div className={styles.moduleGroup} key={group}>
                <h3>{group}</h3>
                <ul className={styles.moduleList}>
                  {modules.map(([title, detail, to]) => (
                    <li key={title}>
                      <h4><Link to={to}>{title}</Link></h4>
                      <p>{detail}</p>
                    </li>
                  ))}
                </ul>
              </div>
            ))}
          </div>
        </section>

        <section className={clsx(styles.section, styles.codeSection)}>
          <div className={styles.sectionIntro}>
            <p className={styles.eyebrow}>{text.codeEyebrow}</p>
            <h2>{text.codeTitle}</h2>
            <p>{text.codeBody}</p>
            <Link className={styles.textLink} to="/docs/getting-started/">
              {text.codeAction} <span aria-hidden="true">→</span>
            </Link>
          </div>
          <div className={styles.codeFrame}>
            <div className={styles.codeCaption}>
              <span>main.cpp</span>
              <span>OpenAI-compatible provider</span>
            </div>
            <CodeBlock language="cpp">{example}</CodeBlock>
          </div>
        </section>

        <section className={styles.section}>
          <div className={styles.sectionIntro}>
            <p className={styles.eyebrow}>{text.useCasesEyebrow}</p>
            <h2>{text.useCasesTitle}</h2>
            <p>{text.useCasesBody}</p>
          </div>
          <div className={styles.useCaseList}>
            {text.useCases.map(([title, detail]) => (
              <article key={title}>
                <h3>{title}</h3>
                <p>{detail}</p>
              </article>
            ))}
          </div>
        </section>

        <section className={styles.closing}>
          <div>
            <p className={styles.eyebrow}>{text.closingEyebrow}</p>
            <h2>{text.closingTitle}</h2>
            <p>{text.closingBody}</p>
          </div>
          <Link className="button button--primary" to="/docs/getting-started/">
            {text.closingAction}
          </Link>
        </section>
      </main>
    </Layout>
  );
}

export default Home;

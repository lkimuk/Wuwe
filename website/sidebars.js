/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  docs: [
    {
      type: 'category',
      label: 'Start here',
      collapsed: false,
      items: ['intro', 'getting-started', 'versioning', 'migration-1.0', 'dependencies', 'packaging'],
    },
    {
      type: 'category',
      label: 'Core runtime',
      collapsed: false,
      items: ['agent-runtime', 'skills', 'agent-host-protocol', 'orchestration', 'reasoning', 'planning', 'multi-agent', 'reflection', 'learning-adaptation', 'training', 'exploration-discovery'],
    },
    {
      type: 'category',
      label: 'Models and tools',
      items: ['llm-providers', 'resource-routing', 'llm-streaming', 'llm-tools', 'filesystem-tools', 'process-tools', 'http-backends'],
    },
    {
      type: 'category',
      label: 'State and knowledge',
      items: ['memory-management', 'memory-deployment', 'knowledge-retrieval'],
    },
    {
      type: 'category',
      label: 'Model Context Protocol',
      items: ['mcp', 'mcp-host-compatibility'],
    },
    {
      type: 'category',
      label: 'Agent interoperability',
      items: ['a2a'],
    },
    {
      type: 'category',
      label: 'Controlled execution',
      items: ['execution-runtime', 'sandbox-architecture'],
    },
    {
      type: 'category',
      label: 'Operations and governance',
      items: ['security-governance', 'guardrails', 'evaluation', 'observability', 'storage-contracts'],
    },
  ],
};

export default sidebars;

import {themes as prismThemes} from 'prism-react-renderer';

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'Wuwe',
  tagline: 'A native C++ framework for composable, auditable agent systems',

  url: 'https://lkimuk.github.io',
  baseUrl: '/Wuwe/',
  organizationName: 'lkimuk',
  projectName: 'Wuwe',
  trailingSlash: true,

  headTags: [
    {
      tagName: 'link',
      attributes: {
        rel: 'icon',
        href: '/Wuwe/favicon.ico',
      },
    },
    {
      tagName: 'link',
      attributes: {
        rel: 'icon',
        type: 'image/png',
        sizes: '64x64',
        href: '/Wuwe/favicon-light.png',
        media: '(prefers-color-scheme: light)',
      },
    },
    {
      tagName: 'link',
      attributes: {
        rel: 'icon',
        type: 'image/png',
        sizes: '64x64',
        href: '/Wuwe/favicon-dark.png',
        media: '(prefers-color-scheme: dark)',
      },
    },
  ],

  onBrokenLinks: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en', 'zh-Hans'],
    localeConfigs: {
      en: {label: 'English', htmlLang: 'en-US'},
      'zh-Hans': {label: '简体中文', htmlLang: 'zh-CN'},
    },
  },

  staticDirectories: ['../docs/assets/brand'],

  presets: [
    [
      'classic',
      {
        docs: {
          path: '../docs',
          routeBasePath: 'docs',
          sidebarPath: './sidebars.js',
          exclude: ['assets/**'],
          showLastUpdateTime: true,
          editUrl: ({docPath}) =>
            `https://github.com/lkimuk/Wuwe/edit/main/docs/${docPath}`,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
        sitemap: {
          changefreq: 'weekly',
          priority: 0.5,
        },
      },
    ],
  ],

  themeConfig: {
    image: 'banner.png',
    metadata: [
      {
        name: 'description',
        content:
          'Wuwe is a C++20 framework for building tool-using, stateful, and auditable AI agents.',
      },
    ],
    colorMode: {
      defaultMode: 'light',
      disableSwitch: false,
      respectPrefersColorScheme: false,
    },
    navbar: {
      title: 'Wuwe',
      hideOnScroll: false,
      logo: {
        alt: 'Wuwe',
        src: 'wuwe-mark-light-transparent.png',
        srcDark: 'wuwe-mark-dark-transparent.png',
      },
      items: [
        {to: '/', label: 'Overview', position: 'left'},
        {to: '/docs/', label: 'Documentation', position: 'left'},
        {
          href: 'https://github.com/lkimuk/Wuwe',
          label: 'GitHub',
          position: 'right',
        },
        {type: 'localeDropdown', position: 'right'},
        {
          to: '/docs/getting-started/',
          label: 'Get started',
          position: 'right',
          className: 'navbar__cta',
        },
      ],
    },
    footer: {
      style: 'light',
      links: [
        {
          title: 'Documentation',
          items: [
            {label: 'Overview', to: '/docs/'},
            {label: 'Get started', to: '/docs/getting-started/'},
            {label: 'Dependencies', to: '/docs/dependencies/'},
            {label: 'Packaging', to: '/docs/packaging/'},
          ],
        },
        {
          title: 'Core systems',
          items: [
            {label: 'Reasoning', to: '/docs/reasoning/'},
            {label: 'Planning', to: '/docs/planning/'},
            {label: 'Memory', to: '/docs/memory-management/'},
            {label: 'MCP', to: '/docs/mcp/'},
          ],
        },
        {
          title: 'Project',
          items: [
            {label: 'GitHub', href: 'https://github.com/lkimuk/Wuwe'},
            {
              label: 'Release 0.1.0',
              href: 'https://github.com/lkimuk/Wuwe/releases',
            },
          ],
        },
      ],
      copyright: 'Copyright © 2026 里缪.',
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'cmake', 'powershell'],
    },
  },
};

export default config;

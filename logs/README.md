# logs/ — AI Coding 日志目录

存放开发中与 AI 工具的对话日志，和作品代码一并提交。

## 目录结构

```text
logs/
└── 1946953767-code/              # GitHub 用户名
    ├── manifest.json             # 会话清单
    └── <date>/                   # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl   # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：本队实际使用的工具为 `dsh`（DeepSeek Harness）。
- 每个 `.jsonl` 每行一个事件，字段对齐[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)第四节。

## 本队日志的来源与导出方式（如实声明）

组委会的日志采集器支持 Claude Code / AIoT-IDE / OpenCode / Codex 四种工具，并要求在被采集
机器上安装 hook、在 openvela 工作区内工作。本队实际使用的是 **DSH（DeepSeek Harness）**，
不在上述工具列表内，因此没有采集器产出的原始日志。

为保证评审可核查，本队做了两件事：

1. **按组委会要求的结构提交日志**：由 DSH 会话记录逐条转换而来，字段对齐手册第四节
   （`text` / `thinking` / `tool_name,input,output` / `model` / `seq`），`seq` 为会话内递增
   序号且无断档。共 **11 个会话、19194 条事件**。对话正文与工具调用**原样搬运，未做改写或
   摘要**，仅过滤了系统自动注入的运行环境提示块（Current runtime context / system-reminder
   等并非人说的话的块），过滤条数记录在转换脚本的运行输出中。
   转换脚本在仓库内，可重跑复核：`tools/ai_logs/export_official_logs.js`。

2. **保留 DSH 原始导出**：未经转换的 `.jsonl`、可读转录、会话索引与收录/排除理由一并放在
   `docs/ai-coding-raw/`，可与本目录内容逐条对照。

我们理解这不等同于官方采集器的产物，评委如需核验，两处记录可以互相印证。

## 安全处理说明：密钥脱敏（Redaction）

开发对话中，用户曾把若干 API 密钥明文粘贴进对话（用于排查第三方接口问题），这些密钥因此
随日志进入了本仓库。本仓库是公开的，为避免密钥公开暴露，本队在提交后做了一次**密钥脱敏**：

- 处理方式：把匹配到的密钥字符串替换为占位符 `<REDACTED-API-KEY>`。
- 处理范围：本目录与 `docs/ai-coding-raw/` 下共 8 个文件、75 处。
- 处理脚本：`tools/ai_logs/redact_secrets.js`（可重跑复核，规则为已知密钥串 + `sk-` 长串兜底）。
- **除此以外，日志内容未做任何改动**：对话的提问、回复、工具调用、时间顺序、`seq` 连续性
  全部保持原样。脱敏只动了密钥字符串本身，不影响日志的真实性与可核查性。

涉事密钥已在对应平台全部吊销（revoke），脱敏属于纵深防御的第二道处理。

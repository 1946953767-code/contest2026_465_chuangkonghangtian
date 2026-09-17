/* 由富抽取的 jsonl 生成两份可读产物：
 *   logs/AI_Coding_日志.md   —— 人类可读转录（用户提问 + AI 回复 + 推理摘要 + 工具调用行）
 *   logs/_session_topics.txt —— 会话清单与提问索引
 */
const fs = require('fs');
const path = require('path');

const PKG = 'C:\\Users\\HUAWEI\\Desktop\\openvela_ai_contest_submission';
const SESS = path.join(PKG, 'logs', 'sessions');
const index = JSON.parse(fs.readFileSync(path.join(PKG, 'logs', '_session_index.json'), 'utf8'));
const inc = index.filter(r => r.included);

function toolLine(name, argsStr) {
  let a = {};
  try { a = JSON.parse(argsStr); } catch (e) {}
  const pick = a.file_path || a.path || a.command || a.pattern || a.remotePath || a.localPath
    || a.alias || a.subagent_id || a.agent_id || a.description || '';
  const extra = pick ? ' ' + String(pick).replace(/\s+/g, ' ').slice(0, 160) : '';
  return '  · `' + name + '`' + extra;
}

let md = [];
md.push('# AI Coding 日志（火箭遥测与地面站系统）');
md.push('');
md.push('> 队伍：创空航天（Team 465）｜ 赛事：2026 首届 openvela AI 硬件开发者大赛');
md.push('> 本文档由 DSH 会话原始记录自动生成，未对内容做人工改写。');
md.push('> 机器可读的完整记录见同目录 `sessions/*.jsonl`（含全部工具调用与命令输出）。');
md.push('> 会话清单、收录/排除理由与统计见 `_session_index.json`。');
md.push('');
md.push('## 收录范围');
md.push('');
md.push('| 会话 | 主题 | 时间 | 提问 | 回复 | 工具调用 |');
md.push('|---|---|---|---|---|---|');
for (const r of inc) {
  md.push('| `' + r.id.slice(0, 20) + '` | ' + r.note + ' | ' + r.mtime + ' | '
    + r.users + ' | ' + r.assistants + ' | ' + r.toolCalls + ' |');
}
md.push('');
md.push('已排除 ' + (index.length - inc.length) + ' 个与本作品无关的会话（SolidWorks 建模、个人事务等），理由逐条记录在 `_session_index.json`。');
md.push('');
md.push('---');
md.push('');

let topics = [];
let totalSteps = 0;

for (const r of inc) {
  const f = path.join(SESS, r.id + '.jsonl');
  if (!fs.existsSync(f)) continue;
  const lines = fs.readFileSync(f, 'utf8').split('\n').filter(Boolean);
  md.push('# 会话：' + (r.title || r.id));
  md.push('');
  md.push('- 会话 ID：`' + r.id + '`');
  md.push('- 时间：' + r.mtime + '　主题：' + r.note);
  md.push('- 规模：提问 ' + r.users + ' / 回复 ' + r.assistants + ' / 工具调用 ' + r.toolCalls + ' / 工具结果 ' + r.toolResults);
  md.push('');
  topics.push('=== ' + (r.title || r.id) + '  [' + r.mtime + ']  ' + r.note);
  const prompts = [];

  for (const l of lines) {
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    if (o.type === 'user/message') {
      const isInjected = /^(Current runtime context|This is an automatically generated checkpoint|<system-reminder>|A skill is a reusable)/.test(o.text.trim());
      if (!isInjected) {
        totalSteps++;
        md.push('### 👤 用户');
        md.push('');
        md.push(o.text);
        md.push('');
        prompts.push(o.text.replace(/\s+/g, ' ').slice(0, 90));
      }
    } else if (o.type === 'assistant/message') {
      if (o.reasoning) {
        md.push('<details><summary>🧠 推理摘要</summary>');
        md.push('');
        md.push('```');
        md.push(o.reasoning.replace(/```/g, '~~~'));
        md.push('```');
        md.push('');
        md.push('</details>');
        md.push('');
      }
      if (o.text) {
        md.push('### 🤖 AI');
        md.push('');
        md.push(o.text);
        md.push('');
      }
    } else if (o.type === 'tool/call') {
      md.push(toolLine(o.name, o.arguments));
    } else if (o.type === 'tool/result') {
      const first = o.text.split('\n').filter(x => x.trim()).slice(0, 2).join(' / ').replace(/\s+/g, ' ').slice(0, 180);
      md.push('    ↳ ' + (first || '(空)') + (o.truncated ? ' …[截断, 原文 ' + o.fullLength + ' 字符]' : ''));
    }
  }
  md.push('');
  md.push('---');
  md.push('');
  for (const p of prompts) topics.push('    · ' + p);
  topics.push('');
}

const mdText = md.join('\n');
fs.writeFileSync(path.join(PKG, 'logs', 'AI_Coding_日志.md'), mdText, 'utf8');
fs.writeFileSync(path.join(PKG, 'logs', '_session_topics.txt'),
  'AI Coding 会话与提问索引（创空航天 / Team 465）\n\n' + topics.join('\n') + '\n', 'utf8');

console.log('AI_Coding_日志.md : ' + (mdText.length / 1048576).toFixed(2) + ' MB, 行数 ' + md.length);
console.log('提问总数: ' + totalSteps);
console.log('会话数: ' + inc.length);

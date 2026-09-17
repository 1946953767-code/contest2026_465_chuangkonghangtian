/* 生成筛选后的可读 AI Coding 日志（Markdown）
 * 过滤规则：
 *  - 丢弃系统注入内容：runtime context / system-reminder / goal_round / <...>
 *  - 丢弃无信息量的口语回复：继续 / 再读一次 / ok / 好 等
 *  - 保留：有技术内容的提问 + 该轮助手的最终回答
 */
const fs = require('fs');
const path = require('path');

const RAW = process.argv[2];
const OUT = process.argv[3];
fs.mkdirSync(OUT, { recursive: true });

const NOISE_PATTERNS = [
  /^Current runtime context/i,
  /^<system-reminder>/i,
  /^<goal_round>/i,
  /^<compacted-summary>/i,
  /^This is an automatically generated checkpoint/i,
  /^\[小咪/,
];
const TRIVIAL = /^(继续|再读一次|好了|好|ok|OK|嗯|行|可以|试试|读|看看|接着|然后呢|你先|收到|明白)[。，!！?？\s]*$/;

function contentText(content) {
  if (!Array.isArray(content)) return '';
  return content.filter(c => c && c.type === 'text').map(c => c.text || '').join('\n').trim();
}

function isNoise(s) {
  if (!s) return true;
  if (NOISE_PATTERNS.some(r => r.test(s))) return true;
  if (s.length < 4) return true;
  if (TRIVIAL.test(s)) return true;
  return false;
}

const files = fs.readdirSync(RAW).filter(f => f.endsWith('.jsonl'));
const sessions = [];

for (const f of files) {
  const lines = fs.readFileSync(path.join(RAW, f), 'utf8').split('\n').filter(Boolean);
  let title = '(无标题)', created = 0;
  const turns = [];
  let cur = null;

  for (const l of lines) {
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    const t = o.type || '';
    const d = o.data || {};
    if (t === 'session' && o.createdAt) created = o.createdAt;
    if (t === 'session/title' && d.title && title === '(无标题)') title = d.title;
    if (t === 'user/message') {
      const s = contentText(d.content);
      cur = { q: s, a: '', tools: [] };
      turns.push(cur);
    } else if (t === 'assistant/message' && cur) {
      const s = contentText(d.content);
      if (s) cur.a += (cur.a ? '\n' : '') + s;
    } else if (t === 'tool/call' && cur) {
      const nm = d.name || d.tool || (d.call && d.call.name);
      if (nm) cur.tools.push(nm);
    }
  }

  const kept = turns.filter(x => !isNoise(x.q));
  if (!kept.length) continue;
  sessions.push({ id: f.replace('.jsonl', ''), title, date: new Date(created).toISOString().slice(0, 16), turns: kept, total: turns.length });
}

sessions.sort((a, b) => (a.date < b.date ? -1 : 1));

let md = '# openvela AI 硬件开发者大赛 · AI Coding 日志\n\n';
md += '> 项目：火箭遥测双端系统（Team 465）\n';
md += '> 工具：DeepSeek Harness（DSH）\n';
md += '> 说明：以下内容摘自本机真实会话记录，已过滤系统注入信息与无信息量的口语回复，仅保留有技术内容的问答。\n\n';
md += '## 日志概览\n\n| 日期 | 会话主题 | 有效问答 | 原始提问 |\n|---|---|---|---|\n';
for (const s of sessions) md += `| ${s.date.slice(0, 10)} | ${s.title} | ${s.turns.length} | ${s.total} |\n`;
md += '\n---\n';

for (const s of sessions) {
  md += `\n## ${s.title}\n\n`;
  md += `- 会话 ID：\`${s.id}\`\n- 时间：${s.date}\n- 有效问答：${s.turns.length} 条\n\n`;
  s.turns.forEach((t, i) => {
    md += `### ${i + 1}. 提问\n\n`;
    md += t.q.split('\n').map(x => '> ' + x).join('\n') + '\n\n';
    if (t.a) {
      const a = t.a.length > 3000 ? t.a.slice(0, 3000) + '\n\n…（回答较长，此处节选）' : t.a;
      md += `**AI 处理与结论：**\n\n${a}\n\n`;
    }
    if (t.tools.length) {
      const uniq = Array.from(new Set(t.tools));
      md += `<sub>工具调用：${uniq.slice(0, 12).join(', ')}${uniq.length > 12 ? ' …' : ''}</sub>\n\n`;
    }
    md += '---\n\n';
  });
}

const outFile = path.join(OUT, 'AI_Coding_日志.md');
fs.writeFileSync(outFile, md, 'utf8');
console.log('已生成: ' + outFile);
console.log('会话数: ' + sessions.length);
console.log('有效问答总数: ' + sessions.reduce((n, s) => n + s.turns.length, 0));
console.log('文件大小: ' + (fs.statSync(outFile).size / 1024).toFixed(0) + ' KB');

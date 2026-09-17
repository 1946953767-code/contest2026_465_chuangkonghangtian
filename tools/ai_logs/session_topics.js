/* 列出所有会话的标题与用户提问，供筛选 */
const fs = require('fs');
const path = require('path');

const dir = process.argv[2];
const perSession = parseInt(process.argv[3] || '30', 10);
const files = fs.readdirSync(dir).filter(f => f.endsWith('.jsonl'));
const out = [];

function texts(content) {
  if (!Array.isArray(content)) return '';
  return content.filter(c => c && c.type === 'text').map(c => c.text || '').join(' ').replace(/\s+/g, ' ').trim();
}

for (const f of files) {
  const lines = fs.readFileSync(path.join(dir, f), 'utf8').split('\n').filter(Boolean);
  let title = '(无标题)', first = 0, questions = [];
  for (const l of lines) {
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    const t = o.type || '';
    if (t === 'session/title' && o.data && o.data.title && title === '(无标题)') title = o.data.title;
    if (t === 'session' && o.createdAt && !first) first = o.createdAt;
    if (t === 'user/message') {
      const s = texts(o.data && o.data.content);
      if (s) questions.push(s);
    }
  }
  out.push({ id: f.replace('.jsonl', ''), title, date: first ? new Date(first).toISOString().slice(0, 16) : '?',
             n: questions.length, questions });
}

out.sort((a, b) => (a.date < b.date ? 1 : -1));
let report = '';
for (const s of out) {
  if (s.n === 0) continue;
  report += '\n================================================\n';
  report += `${s.date}  |  ${s.n} 次提问  |  ${s.id}\n`;
  report += `标题: ${s.title}\n`;
  report += '------------------------------------------------\n';
  s.questions.slice(0, perSession).forEach((q, i) => {
    report += `  ${String(i + 1).padStart(2)}. ${q.length > 120 ? q.slice(0, 120) + '…' : q}\n`;
  });
  if (s.questions.length > perSession) report += `  ...（还有 ${s.questions.length - perSession} 条）\n`;
}
fs.writeFileSync(path.join(dir, '..', '_session_topics.txt'), report, 'utf8');
console.log(report.slice(0, 6000));
console.log('\n完整清单已写入 logs/_session_topics.txt');

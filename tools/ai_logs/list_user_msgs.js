/* 把会话里的用户提问按顺序列出来，便于筛选主题 */
const fs = require('fs');
const path = require('path');

const dir = process.argv[2];
const limit = parseInt(process.argv[3] || '80', 10);
const files = fs.readdirSync(dir).filter(f => f.endsWith('.jsonl'));

for (const f of files) {
  const p = path.join(dir, f);
  const size = fs.statSync(p).size;
  if (size < 20000) continue;
  console.log('\n============================================');
  console.log('会话 ' + f.replace('.jsonl', '') + '  (' + (size / 1024).toFixed(0) + ' KB)');
  console.log('============================================');
  const lines = fs.readFileSync(p, 'utf8').split('\n').filter(Boolean);
  let n = 0;
  for (const l of lines) {
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    if ((o.type || '') !== 'user/message') continue;
    let t = (o.text || o.content || '');
    if (typeof t !== 'string') t = JSON.stringify(t);
    t = t.replace(/\s+/g, ' ').trim();
    if (!t) continue;
    n++;
    if (n > limit) { console.log('  ...(还有更多)'); break; }
    console.log(String(n).padStart(3) + '. ' + (t.length > 160 ? t.slice(0, 160) + '…' : t));
  }
}

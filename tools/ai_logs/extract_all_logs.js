/* 批量提取 DSH 多帧 zstd 会话日志，输出可读文本并统计项目相关内容 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = 'C:\\Users\\HUAWEI\\.dsh\\sessions\\--C-Users-HUAWEI-Desktop--';
const OUT_DIR = 'C:\\Users\\HUAWEI\\Desktop\\openvela_ai_contest_submission\\logs\\raw';
const MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);
const KEYWORDS = ['ESP32', '思澈', 'SF32', 'LoRa', '遥测', 'central_tx',
                  'ground_station', 'uart2hwtest', '雷达', 'BME280', 'MPU6050',
                  'nuttx', 'openvela', 'sftool'];

fs.mkdirSync(OUT_DIR, { recursive: true });

function walk(dir, acc) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, acc);
    else if (/session\.jsonl\.zst(d)?$/.test(e.name)) acc.push(p);
  }
  return acc;
}

function extractFrames(buf) {
  const offs = [];
  let i = 0;
  while (true) {
    const p = buf.indexOf(MAGIC, i);
    if (p < 0) break;
    offs.push(p); i = p + 4;
  }
  const parts = [];
  for (let k = 0; k < offs.length; k++) {
    const start = offs[k];
    const end = k + 1 < offs.length ? offs[k + 1] : buf.length;
    try { parts.push(zlib.zstdDecompressSync(buf.subarray(start, end)).toString('utf8')); } catch (e) {}
  }
  return parts.join('');
}

const files = walk(ROOT, []);
const index = [];

for (const f of files) {
  const id = path.basename(path.dirname(f));
  let text = '';
  try { text = extractFrames(fs.readFileSync(f)); } catch (e) { continue; }
  const lines = text.split('\n').filter(Boolean);
  if (!lines.length) continue;

  let users = 0, assistants = 0, tools = 0;
  const found = new Set();
  for (const k of KEYWORDS) if (text.includes(k)) found.add(k);
  const msgs = [];
  for (const l of lines) {
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    const t = o.type || '';
    if (t === 'user/message') { users++; msgs.push({ role: 'user', text: (o.text || o.content || '').toString() }); }
    else if (t === 'assistant/message') { assistants++; msgs.push({ role: 'assistant', text: (o.text || o.content || '').toString() }); }
    else if (t === 'tool/call') tools++;
  }
  const hits = Array.from(found).reduce((n, k) => n + text.split(k).length - 1, 0);
  index.push({ id, file: f, textSize: text.length, users, assistants, tools,
               hits, keywords: Array.from(found), mtime: fs.statSync(f).mtime.toISOString().slice(0, 16) });

  if (hits > 0) {
    fs.writeFileSync(path.join(OUT_DIR, id + '.jsonl'), text, 'utf8');
  }
}

index.sort((a, b) => b.hits - a.hits || b.textSize - a.textSize);
fs.writeFileSync(path.join(OUT_DIR, '..', '_session_index.json'), JSON.stringify(index, null, 2), 'utf8');

console.log('会话总数: ' + files.length + '，有内容的: ' + index.length);
console.log('\n=== 项目相关会话（按关键词命中排序）===');
for (const r of index) {
  if (r.hits === 0) continue;
  console.log(`${String(r.hits).padStart(5)} 次 | ${(r.textSize / 1024).toFixed(0).padStart(4)}KB | 提问 ${r.users} / 回答 ${r.assistants} / 工具 ${r.tools} | ${r.mtime} | ${r.id}`);
}
const rel = index.filter(r => r.hits > 0);
console.log('\n相关会话数: ' + rel.length + '，已导出到 logs/raw/');

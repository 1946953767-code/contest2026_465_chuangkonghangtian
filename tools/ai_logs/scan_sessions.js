const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = 'C:\\Users\\HUAWEI\\.dsh\\sessions\\--C-Users-HUAWEI-Desktop--';
const KEYWORDS = ['ESP32', '思澈', 'SF32', 'LoRa', '遥测', 'central_tx',
                  'ground_station', 'uart2hwtest', '雷达', 'BME280', 'MPU6050'];

function walk(dir, acc) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, acc);
    else if (/session\.jsonl\.zst(d)?$/.test(e.name)) acc.push(p);
  }
  return acc;
}

const files = walk(ROOT, []);
console.log('会话日志文件数: ' + files.length);

const rows = [];
for (const f of files) {
  let raw, text;
  try {
    raw = fs.readFileSync(f);
    text = zlib.zstdDecompressSync(raw).toString('utf8');
  } catch (e) { continue; }
  const lines = text.split('\n').filter(Boolean);
  let hits = 0;
  const found = new Set();
  for (const k of KEYWORDS) if (text.includes(k)) { hits += text.split(k).length - 1; found.add(k); }
  rows.push({
    f, rawSize: raw.length, textSize: text.length, lines: lines.length,
    hits, found: Array.from(found),
    mtime: fs.statSync(f).mtime.toISOString().slice(0, 16)
  });
}

rows.sort((a, b) => b.hits - a.hits || b.textSize - a.textSize);
console.log('\n=== 按项目关键词命中数排序（前 15）===');
for (const r of rows.slice(0, 15)) {
  if (r.hits === 0) break;
  console.log(`${String(r.hits).padStart(5)} 次 | ${(r.textSize / 1024).toFixed(0).padStart(5)}KB | ${r.lines} 行 | ${r.mtime} | ${path.basename(path.dirname(r.f))}`);
  console.log('        关键词: ' + r.found.join(' '));
}
console.log('\n总计有空日志: ' + rows.filter(r => r.textSize < 300).length + ' 个（无对话内容）');
fs.writeFileSync('C:\\Users\\HUAWEI\\Desktop\\openvela_ai_contest_submission\\logs\\_session_index.json',
  JSON.stringify(rows, null, 2), 'utf8');
console.log('索引已写入 logs/_session_index.json');

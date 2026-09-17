const fs = require('fs');
const zlib = require('zlib');

const src = process.argv[2];
const buf = fs.readFileSync(src);

// zstd 帧魔数 0x28 0xB5 0x2F 0xFD
const magic = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);
const offsets = [];
let i = 0;
while (i >= 0 && i < buf.length) {
  const p = buf.indexOf(magic, i);
  if (p < 0) break;
  offsets.push(p);
  i = p + 4;
}
console.log('文件大小: ' + buf.length);
console.log('zstd 帧数: ' + offsets.length);
console.log('前 10 帧位置: ' + offsets.slice(0, 10).join(', '));

// 逐帧解压
let total = 0, frames = 0;
const parts = [];
for (let k = 0; k < offsets.length; k++) {
  const start = offsets[k];
  const end = (k + 1 < offsets.length) ? offsets[k + 1] : buf.length;
  try {
    const d = zlib.zstdDecompressSync(buf.subarray(start, end));
    total += d.length;
    frames++;
    parts.push(d.toString('utf8'));
  } catch (e) { /* 帧尾可能不完整，跳过 */ }
}
console.log('成功解压帧数: ' + frames + ', 总解压字节: ' + total);
const text = parts.join('');
fs.writeFileSync(process.argv[3], text, 'utf8');
const lines = text.split('\n').filter(Boolean);
console.log('合并后行数: ' + lines.length);
if (lines.length) {
  const types = {};
  for (const l of lines) {
    try { const o = JSON.parse(l); const t = o.type || o.role || '?'; types[t] = (types[t] || 0) + 1; }
    catch (e) { types['<err>'] = (types['<err>'] || 0) + 1; }
  }
  console.log('行类型: ' + JSON.stringify(types));
}

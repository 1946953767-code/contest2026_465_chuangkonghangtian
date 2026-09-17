/* 统计 AI Coding 用量（模型调用次数、各模型 token 明细），用于填写报告 3.6「AI-Native 开发说明」。
 * 数据源：DSH 会话原始记录中的 assistant/message.data.usage。
 * 用法：node token_stats.js            —— 统计作品相关会话
 *       node token_stats.js --all      —— 统计全部会话
 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = 'C:\\Users\\HUAWEI\\.dsh\\sessions\\--C-Users-HUAWEI-Desktop--';
const MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);

// 与本作品直接相关的会话（其余会话为 SolidWorks 建模、个人事务等无关内容）
const RELEVANT = new Set([
  'session-2703602a-9e23-4573-a761-39b99ff21934',
  'session-d86bf393-c541-4edb-b5df-575745eecc65',
  'session-fe991a78-619f-4810-bc66-da67c3f1ce97',
  'session-21ba56e7-e866-4778-a3e2-5748f0b5311c',
  '821e9506-15bb-49f9-9d04-090e174e5ddd',
  'de9d87bb-5e73-4b15-837c-d7e5029f7f2e',
  '0c100b21-6dc7-443f-a913-9adb793f0b06',
  'session-e23b1704-3e9a-48ed-a302-ebfd8c766c0d',
  'session-e4aa8bbe-a777-492e-98a2-9dd35bd7b6df',
  'session-03d0b494-04de-4bff-84d6-f4ee466e8a49',
  'session-5827a966-72cc-4b06-aee0-038a1ffed337',
]);

const ALL = process.argv.includes('--all');

function frames(buf) {
  const offs = [];
  let i = 0;
  while (true) {
    const p = buf.indexOf(MAGIC, i);
    if (p < 0) break;
    offs.push(p);
    i = p + 4;
  }
  const parts = [];
  for (let k = 0; k < offs.length; k++) {
    const end = k + 1 < offs.length ? offs[k + 1] : buf.length;
    try { parts.push(zlib.zstdDecompressSync(buf.subarray(offs[k], end)).toString('utf8')); } catch (e) {}
  }
  return parts.join('');
}

const byModel = new Map();
const byProv = new Map();

function bump(map, key, u) {
  const s = map.get(key) || { calls: 0, input: 0, output: 0, cacheRead: 0, reasoning: 0 };
  s.calls++;
  s.input += u.inputTokens || 0;
  s.output += u.outputTokens || 0;
  s.cacheRead += u.cacheReadTokens || 0;
  s.reasoning += u.reasoningTokens || 0;
  map.set(key, s);
}

let sessionsUsed = 0;
for (const dir of fs.readdirSync(ROOT)) {
  if (!ALL && !RELEVANT.has(dir)) continue;
  const f = path.join(ROOT, dir, 'session.jsonl.zstd');
  if (!fs.existsSync(f)) continue;
  sessionsUsed++;
  for (const l of frames(fs.readFileSync(f)).split('\n')) {
    if (!l) continue;
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    if (o.type !== 'assistant/message') continue;
    const u = o.data && o.data.usage;
    if (!u) continue;
    const src = (o.data && o.data.message && o.data.message.source) || {};
    bump(byModel, src.model || '(未知)', u);
    bump(byProv, src.provider || '(未知)', u);
  }
}

function table(title, map) {
  console.log('=== ' + title + ' ===');
  const rows = [...map.entries()].sort((a, b) => b[1].input - a[1].input);
  const t = { calls: 0, input: 0, output: 0, cacheRead: 0, reasoning: 0 };
  console.log('名称'.padEnd(28) + '调用'.padStart(7) + '输入token'.padStart(13)
    + '输出token'.padStart(12) + '缓存读token'.padStart(14) + '推理token'.padStart(12));
  for (const [k, s] of rows) {
    console.log(k.padEnd(28) + String(s.calls).padStart(7) + String(s.input).padStart(13)
      + String(s.output).padStart(12) + String(s.cacheRead).padStart(14) + String(s.reasoning).padStart(12));
    for (const key of Object.keys(t)) t[key] += s[key];
  }
  console.log('-'.repeat(86));
  console.log('合计'.padEnd(28) + String(t.calls).padStart(7) + String(t.input).padStart(13)
    + String(t.output).padStart(12) + String(t.cacheRead).padStart(14) + String(t.reasoning).padStart(12));
  console.log('');
  return t;
}

console.log('统计范围: ' + (ALL ? '全部会话' : '作品相关的 11 个会话') + '（实际读取 ' + sessionsUsed + ' 个会话）\n');
const m = table('按模型', byModel);
const p = table('按提供方', byProv);

console.log('=== 报告 3.6 可直接引用的数据 ===');
console.log('模型调用次数      : ' + m.calls);
console.log('输入 token        : ' + m.input);
console.log('输出 token        : ' + m.output);
console.log('输入 + 输出       : ' + (m.input + m.output));
console.log('缓存读 token      : ' + m.cacheRead);
console.log('含缓存读合计      : ' + (m.input + m.output + m.cacheRead));

let mi = 0, mo = 0, mr = 0, mc = 0;
for (const [k, s] of byModel) {
  if (/mimo/i.test(k)) { mi += s.input; mo += s.output; mr += s.cacheRead; mc += s.calls; }
}
if (mc) {
  console.log('');
  console.log('★ 大赛 MiMo 专项（' + mc + ' 次调用）：');
  console.log('   输入 ' + mi + ' + 输出 ' + mo + ' = ' + (mi + mo));
  console.log('   含缓存读合计 ' + (mi + mo + mr));
}

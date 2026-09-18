// 密钥脱敏工具：把仓库文本中形如 sk-xxxx 的 API 密钥替换为占位符。
//
// 背景：AI Coding 日志如实收录了开发对话，而对话中用户曾明文粘贴过 API 密钥；
//       仓库是公开的，会把密钥暴露出去。此脚本在导出/提交前做一道脱敏。
//
// 用法：
//   node redact_secrets.js                    # 处理默认仓库路径
//   node redact_secrets.js <repoDir> --dry    # 只看会改什么，不写盘
//
// 说明：规则是「已知密钥串精确匹配 + sk- 长串兜底」。脱敏只替换密钥字符串本身，
//       不触碰对话正文、工具调用与 seq 顺序，因此不影响日志真实性。
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const DRY = args.includes('--dry');
const REPO = args.find((a) => !a.startsWith('--')) ||
  path.join(__dirname, '..', '..');

// 已知密钥（脱敏后此处留空，重跑时依赖兜底规则即可）
const KNOWN = [];

const GENERIC = /sk-[A-Za-z0-9]{32,80}/g;
const PLACEHOLDER = '<REDACTED-API-KEY>';

function walk(dir, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.name === '.git') continue;
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (/\.(md|txt|jsonl|json|js|py|c|h)$/i.test(e.name)) out.push(p);
  }
  return out;
}

let changed = 0;
let hits = 0;

for (const f of walk(REPO)) {
  let text;
  try { text = fs.readFileSync(f, 'utf8'); } catch { continue; }
  const before = text;
  let n = 0;
  for (const s of KNOWN) {
    const parts = text.split(s);
    if (parts.length > 1) { n += parts.length - 1; text = parts.join(PLACEHOLDER); }
  }
  text = text.replace(GENERIC, () => { n++; return PLACEHOLDER; });
  if (text !== before) {
    changed++; hits += n;
    console.log('  ' + path.relative(REPO, f) + '   ' + n + ' 处');
    if (!DRY) fs.writeFileSync(f, text, 'utf8');
  }
}

console.log((DRY ? '[DRY RUN] ' : '') + '命中 ' + changed + ' 个文件，共 ' + hits + ' 处');

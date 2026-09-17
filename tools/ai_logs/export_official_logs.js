/* 把 DSH 会话记录转换成组委会《AI Coding 日志归集与提交手册》要求的结构：
 *
 *   logs/<github_login>/manifest.json
 *   logs/<github_login>/<YYYY-MM-DD>/<tool>__<sid>.jsonl
 *
 * 事件字段对齐手册第四节「记录字段」：
 *   text / thinking / tool_name + input + output / model (+tokens_in/out 若有) / seq
 *
 * 原则：对话内容逐条搬运，不改写、不摘要；只丢掉系统自动注入的运行环境提示
 *      （Current runtime context / system-reminder / checkpoint 等，不是人说的话）。
 *
 * 用法：node export_official_logs.js [github_login]
 *       默认 github_login = 1946953767-code
 */
const fs = require('fs');
const path = require('path');

const REPO = path.resolve(__dirname, '..', '..');
const RAW = path.join(REPO, 'docs', 'ai-coding-raw');
const LOGIN = process.argv[2] || '1946953767-code';
const TOOL = 'dsh';
const TEAM_ID = 'contest2026_465_chuangkonghangtian';

const TZ = 'Asia/Shanghai';
function dayKey(ms){
  const d = new Date(ms);
  const p = new Intl.DateTimeFormat('en-CA', { timeZone: TZ, year: 'numeric', month: '2-digit', day: '2-digit' });
  return p.format(d);                                  // YYYY-MM-DD
}
function iso(ms){
  const d = new Date(ms);
  const p = new Intl.DateTimeFormat('sv-SE', {
    timeZone: TZ, year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
  return p.format(d).replace(' ', 'T') + '+08:00';
}

const INJECTED = /^(Current runtime context|This is an automatically generated checkpoint|A skill is a reusable|<system-reminder>|<available_skills>|<skill_content)/;

const index = JSON.parse(fs.readFileSync(path.join(RAW, '_session_index.json'), 'utf8'));
const included = index.filter(r => r.included);

const outRoot = path.join(REPO, 'logs', LOGIN);
if (fs.existsSync(path.join(REPO, 'logs'))) {
  // 只清掉本工具上次生成的 <date> 目录与 manifest，不动别的
  for (const e of fs.readdirSync(path.join(REPO, 'logs'), { withFileTypes: true })){
    if (e.isDirectory() && e.name === LOGIN) fs.rmSync(path.join(REPO, 'logs', LOGIN), { recursive: true, force: true });
  }
}

const sessions = [];
let totalEvents = 0;

for (const rec of included){
  const src = path.join(RAW, 'sessions', rec.id + '.jsonl');
  if (!fs.existsSync(src)) { console.log('缺文件，跳过: ' + rec.id); continue; }

  const sid = rec.id.replace(/^session-/, '');
  const rows = fs.readFileSync(src, 'utf8').split('\n').filter(Boolean);

  const events = [];
  let seq = 0, first = null, last = null, dropped = 0;

  for (const line of rows){
    let o; try { o = JSON.parse(line); } catch (e) { continue; }
    if (typeof o.time === 'number'){
      if (first === null || o.time < first) first = o.time;
      if (last === null || o.time > last) last = o.time;
    }

    if (o.type === 'user/message'){
      const t = (o.text || '').trim();
      if (!t || INJECTED.test(t)) { dropped++; continue; }
      events.push({ seq: ++seq, ts: iso(o.time), role: 'user', text: o.text });

    } else if (o.type === 'assistant/message'){
      const ev = { seq: ++seq, ts: iso(o.time), role: 'assistant' };
      if (o.text) ev.text = o.text;
      if (o.reasoning) ev.thinking = o.reasoning;
      if (o.model) ev.model = o.model;
      const u = o.usage || (o.data && o.data.usage);
      if (u){
        if (typeof u.inputTokens === 'number') ev.tokens_in = u.inputTokens;
        if (typeof u.outputTokens === 'number') ev.tokens_out = u.outputTokens;
      }
      if (!ev.text && !ev.thinking) { seq--; continue; }   // 空消息不入档
      events.push(ev);

    } else if (o.type === 'tool/call'){
      if (!o.name) continue;
      const ev = { seq: ++seq, ts: iso(o.time), role: 'assistant', tool_name: o.name };
      let args = o.arguments;
      try { args = JSON.parse(args); } catch (e) { /* 保留原字符串 */ }
      ev.input = args === undefined ? null : args;
      events.push(ev);

    } else if (o.type === 'tool/result'){
      const ev = { seq: ++seq, ts: iso(o.time), role: 'tool', output: o.text === undefined ? '' : o.text };
      if (o.truncated) { ev.truncated = true; ev.full_length = o.fullLength; }
      events.push(ev);
    }
    // session / session/title 等元信息不入档
  }

  if (!events.length) { console.log('无有效事件，跳过: ' + sid); continue; }

  const day = dayKey(first);
  const rel = path.join('logs', LOGIN, day, TOOL + '__' + sid + '.jsonl');
  const dst = path.join(REPO, rel);
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.writeFileSync(dst, events.map(e => JSON.stringify(e)).join('\n') + '\n', 'utf8');

  sessions.push({
    session_id: rec.id,
    tool: TOOL,
    started_at: iso(first),
    last_event_at: iso(last),
    event_count: events.length,
    file_path: rel.split(path.sep).join('/'),
    collection_mode: 'cli',
    health: 'ok'
  });
  totalEvents += events.length;
  console.log(day + '  ' + String(events.length).padStart(6) + ' 事件  丢注入 ' + String(dropped).padStart(4)
    + '  ' + rec.note + '   -> ' + rel);
}

sessions.sort((a, b) => a.started_at < b.started_at ? -1 : 1);

fs.writeFileSync(path.join(outRoot, 'manifest.json'), JSON.stringify({
  schema_version: '1.0',
  team_id: TEAM_ID,
  github_login: LOGIN,
  generator: 'dsh-export/1.0 (DeepSeek Harness 会话记录转换；token 用量按会话汇总见 docs/技术报告 3.6)',
  sessions,
  updated_at: iso(Date.now())
}, null, 2) + '\n', 'utf8');

console.log('\n会话数 ' + sessions.length + '，事件总数 ' + totalEvents);
console.log('输出目录 ' + outRoot);

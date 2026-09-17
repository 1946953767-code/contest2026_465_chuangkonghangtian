/* 富抽取：保留对话正文 + 工具调用 + 工具结果（截断超大输出），丢弃纯重复的流式分片。
 * 产出 logs/sessions/<id>.jsonl（机器可读原始记录）与 _session_index.json（含入选/排除理由）。 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = 'C:\\Users\\HUAWEI\\.dsh\\sessions\\--C-Users-HUAWEI-Desktop--';
const OUT = 'C:\\Users\\HUAWEI\\Desktop\\openvela_ai_contest_submission\\logs\\sessions';
const MAGIC = Buffer.from([0x28, 0xb5, 0x2f, 0xfd]);

const ARG_CAP = 1200;   // 工具调用参数截断
const RES_CAP = 2500;   // 工具结果截断
const THINK_CAP = 500;  // 推理过程截断（仅留思路片段）

const RELEVANT = {
  'session-2703602a-9e23-4573-a761-39b99ff21934': 'ESP32-S3 传感器底板设计',
  'session-d86bf393-c541-4edb-b5df-575745eecc65': 'ESP32-S3 无源蜂鸣器测试',
  'session-fe991a78-619f-4810-bc66-da67c3f1ce97': 'ESP32 OLED 短接后恢复',
  'session-21ba56e7-e866-4778-a3e2-5748f0b5311c': 'EDA 桥接与工具链配置',
  '821e9506-15bb-49f9-9d04-090e174e5ddd': 'SF32LB52 触摸屏调试',
  'de9d87bb-5e73-4b15-837c-d7e5029f7f2e': '任务上下文复核',
  '0c100b21-6dc7-443f-a913-9adb793f0b06': 'openvela/NuttX 串口设备确定',
  'session-e23b1704-3e9a-48ed-a302-ebfd8c766c0d': '开发环境搭建 + 板级驱动',
  'session-e4aa8bbe-a777-492e-98a2-9dd35bd7b6df': '桌面代码整理与交接',
  'session-03d0b494-04de-4bff-84d6-f4ee466e8a49': '地面站固件主体开发',
  // session-5827a966：遥测/雷达/网页大屏开发 + SD 卡驱动排查与 RGB 指示灯。
  // 其中早期的"箭载 LoRa 链路存活检测"尝试最终被改为默认关闭的开关，
  // 属于真实开发过程的一部分，保留在日志里（不是对外宣称的成果）。
  'session-5827a966-72cc-4b06-aee0-038a1ffed337': '遥测/雷达/网页大屏 + SD 驱动与 RGB',
};

const EXCLUDED_WHY = {
  '3d9f5f69-525e-4520-88bc-dc2ec2d6b6ea': 'SolidWorks 机械建模，与作品无关',
  'ba982758-7237-46ab-8aba-9c5891c18642': 'SolidWorks 机械建模，与作品无关',
  'bfbe1bea-0360-4cc1-8d11-8c0960f63a5f': 'SolidWorks 机械建模，与作品无关',
  'abdbb6d6-1586-4642-8295-4ca2a6e06cb0': 'SolidWorks 机械建模，与作品无关',
  'session-663f55fc-9f6e-430b-8cbf-b56dfe7f14d2': '只问 dsh 版本，无作品内容',
  'session-5d9435b6-efe2-4bcb-9e12-e7f184e4acba': '标题「11」，无作品内容',
  'session-d9aa6fdb-5cab-42a1-91aa-ce8b93431587': '闲聊，无作品内容',
  'session-9623475a-97dd-47e3-a572-ae2643365bd7': '手机网页打开方法，非本项目',
  'session-bf36c058-f291-49b2-bc1d-4e98a8bd79c6': '模型选型咨询，非本项目',
  'session-e26f2fda-f573-4ccf-a5d6-f8fa30e4d70c': '华为产品介绍，无关',
  'session-0c3f64f7-e062-498c-91b0-79a3c76ccbb5': 'GitHub 访问问题，无关',
  'session-6842d226-17aa-453b-b09c-6405c42d259c': '标题「1111」，无关',
  'session-efd68f95-49b1-4c3e-9957-a0ecd2819bf1': '身高体重咨询，无关',
  'session-75af61f0-0ce6-4964-9342-0439a4062ffe': '团委应聘书，无关',
  'session-0ce7237f-d7a1-452d-89a0-8238ab9752df': 'DeepSeek API 文档，无关',
  'session-6795b5c4-8cb7-4dcd-a8c3-a63cf24dfea3': '个人信息表，无关',
  'session-d6191239-9c52-4869-a056-33596a7c2e9c': '入党申请书，无关',
  '44336f7b-6725-4f57-9da3-ac7e1ac3dcf8': '与 21ba56e7 重复的并行会话，去重',
  'f861037d-e47c-44b1-b5fa-21362c3402bb': '与 21ba56e7 重复的并行会话，去重',
};

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

function texts(content) {
  if (!Array.isArray(content)) return '';
  return content.filter(x => x && x.type === 'text').map(x => x.text || '').join('\n').trim();
}

function resultText(o) {
  const msg = o.data && o.data.message;
  if (!msg || !Array.isArray(msg.content)) return '';
  const out = [];
  for (const block of msg.content) {
    if (!block || block.type !== 'tool-result' || !Array.isArray(block.content)) continue;
    for (const c of block.content) {
      if (c && c.type === 'text') out.push(c.text || '');
      else if (c && c.type === 'image') out.push('[图片]');
    }
  }
  return out.join('\n').trim();
}

fs.mkdirSync(OUT, { recursive: true });
const index = [];
let grand = 0;

for (const dir of fs.readdirSync(ROOT)) {
  const f = path.join(ROOT, dir, 'session.jsonl.zstd');
  if (!fs.existsSync(f)) continue;

  const text = frames(fs.readFileSync(f));
  const kept = [];
  let title = '', created = '', cwd = '';
  let nUser = 0, nAsst = 0, nCall = 0, nRes = 0, truncated = 0;
  const toolsUsed = new Map();

  for (const l of text.split('\n')) {
    if (!l) continue;
    let o; try { o = JSON.parse(l); } catch (e) { continue; }
    const t = o.type || '';

    if (t === 'session') {
      created = String(o.createdAt || ''); cwd = o.cwd || '';
      kept.push(JSON.stringify({ type: 'session', id: o.id, createdAt: created, cwd }));
    } else if (t === 'session/title') {
      title = (o.data && o.data.title) || title;
      kept.push(JSON.stringify({ type: 'session/title', title }));
    } else if (t === 'user/message') {
      const s = texts(o.data && o.data.content);
      if (!s) continue;
      nUser++;
      kept.push(JSON.stringify({ type: 'user/message', time: o.time, text: s }));
    } else if (t === 'assistant/message') {
      const msg = o.data && o.data.message;
      const s = texts(msg && msg.content);
      const parts = (msg && Array.isArray(msg.content)) ? msg.content : [];
      const think = parts.filter(x => x && x.type === 'reasoning')
                         .map(x => x.text || '').join('\n').trim();
      if (!s && !think) continue;
      nAsst++;
      let thinkOut = think;
      let thinkCut = false;
      if (thinkOut.length > THINK_CAP) { thinkOut = thinkOut.slice(0, THINK_CAP); thinkCut = true; truncated++; }
      kept.push(JSON.stringify({ type: 'assistant/message', time: o.time,
                                 text: s,
                                 reasoning: thinkOut || undefined,
                                 reasoningChars: think.length,
                                 reasoningTruncated: thinkCut || undefined,
                                 model: (msg && msg.source && msg.source.model) || undefined }));
    } else if (t === 'tool/call') {
      const d = o.data || {};
      let args = String(d.arguments || '');
      let cut = false;
      if (args.length > ARG_CAP) { args = args.slice(0, ARG_CAP); cut = true; truncated++; }
      nCall++;
      toolsUsed.set(d.name, (toolsUsed.get(d.name) || 0) + 1);
      kept.push(JSON.stringify({ type: 'tool/call', time: o.time, name: d.name,
                                 arguments: args, argumentsTruncated: cut }));
    } else if (t === 'tool/result') {
      let s = resultText(o);
      if (!s) continue;
      const full = s.length;
      let cut = false;
      if (s.length > RES_CAP) { s = s.slice(0, RES_CAP); cut = true; truncated++; }
      nRes++;
      kept.push(JSON.stringify({ type: 'tool/result', time: o.time, text: s,
                                 fullLength: full, truncated: cut }));
    } else if (t === 'compaction/summary') {
      const s = JSON.stringify(o.data || {}).slice(0, 3000);
      kept.push(JSON.stringify({ type: 'compaction/summary', text: s }));
    } else if (t === 'todo/write') {
      kept.push(JSON.stringify({ type: 'todo/write', text: JSON.stringify(o.data || {}).slice(0, 2000) }));
    } else if (t === 'goal/change') {
      kept.push(JSON.stringify({ type: 'goal/change', text: JSON.stringify(o.data || {}).slice(0, 1000) }));
    }
  }

  if (!kept.length) continue;
  const body = kept.join('\n') + '\n';
  const isRel = !!RELEVANT[dir];

  index.push({
    id: dir, title, createdAt: created, mtime: fs.statSync(f).mtime.toISOString().slice(0, 16),
    zstdBytes: fs.statSync(f).size, expandedBytes: text.length,
    exportedBytes: body.length,
    users: nUser, assistants: nAsst, toolCalls: nCall, toolResults: nRes,
    truncatedItems: truncated,
    included: isRel,
    note: isRel ? RELEVANT[dir] : (EXCLUDED_WHY[dir] || '未纳入'),
    topTools: [...toolsUsed.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8),
  });

  if (isRel) {
    fs.writeFileSync(path.join(OUT, dir + '.jsonl'), body, 'utf8');
    grand += body.length;
    console.log('%8.2f MB -> %6.2f MB  提问%4d 回答%4d 工具%4d/%4d  截断%d  %s'
      .replace('%8.2f', String((text.length / 1048576).toFixed(2)).padStart(8))
      .replace('%6.2f', String((body.length / 1048576).toFixed(2)).padStart(6))
      .replace('%4d', String(nUser).padStart(4))
      .replace('%4d', String(nAsst).padStart(4))
      .replace('%4d', String(nCall).padStart(4))
      .replace('%4d', String(nRes).padStart(4))
      .replace('%d', String(truncated).padStart(4))
      + '  ' + dir);
  }
}

index.sort((a, b) => (b.included - a.included) || (b.expandedBytes - a.expandedBytes));
fs.writeFileSync(path.join(OUT, '..', '_session_index.json'), JSON.stringify(index, null, 2), 'utf8');
const inc = index.filter(r => r.included);
console.log('\n收录 ' + inc.length + ' 个会话，共 ' + (grand / 1048576).toFixed(2) + ' MB');
console.log('排除 ' + (index.length - inc.length) + ' 个会话（理由已写入 _session_index.json）');

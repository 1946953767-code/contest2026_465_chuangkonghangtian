const fs = require('fs');
const p = process.argv[2];
const lines = fs.readFileSync(p, 'utf8').split('\n').filter(Boolean);
const seen = {};
let shown = 0;
for (const l of lines) {
  let o; try { o = JSON.parse(l); } catch (e) { continue; }
  const t = o.type || '?';
  if (seen[t]) continue;
  seen[t] = 1;
  console.log('=== type: ' + t + ' ===');
  console.log(JSON.stringify(o).slice(0, 600));
  console.log('');
  if (++shown > 14) break;
}

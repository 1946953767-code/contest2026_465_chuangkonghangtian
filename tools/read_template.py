import zipfile
import re
import sys

path = r'C:\Users\HUAWEI\Desktop\2026 首届 openvela AI 硬件开发者大赛 - 作品提交模板.docx'
out = r'C:\Users\HUAWEI\Desktop\openvela_ai_contest_submission\docs\_模板原文.txt'

z = zipfile.ZipFile(path)
names = [n for n in z.namelist() if n.endswith('.xml')]
xml = z.read('word/document.xml').decode('utf-8', 'replace')

# 按段落切分，保留表格结构提示
paras = re.findall(r'<w:p[ >].*?</w:p>', xml, re.S)
lines = []
for p in paras:
    texts = re.findall(r'<w:t[^>]*>(.*?)</w:t>', p, re.S)
    s = ''.join(texts)
    s = re.sub(r'&amp;', '&', s)
    s = re.sub(r'&lt;', '<', s)
    s = re.sub(r'&gt;', '>', s)
    s = s.strip()
    if s:
        lines.append(s)

text = '\n'.join(lines)
with open(out, 'w', encoding='utf-8') as f:
    f.write(text)

print('段落数:', len(lines))
print('字符数:', len(text))
print()
print(text[:4000])

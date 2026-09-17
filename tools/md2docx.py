"""把 Markdown 技术报告转成 .docx（不依赖第三方库，直接生成 OOXML）。

排版约定（中文公文/技术报告习惯）：
  · 中文正文 仿宋_GB2312，西文 Times New Roman，小四(12pt)，行距 1.5，首行缩进 2 字符
  · 标题 黑体，一级小二 / 二级三号 / 三级四号 / 四级小四
  · 表格 仿宋_GB2312 五号(10.5pt)，表头加粗居中、浅灰底纹、全框线
  · 代码 Consolas 小五(9pt)，浅灰底纹
  · 把 ✅❌⚠️ 之类的符号转成文字，避免文档里出现彩色 emoji
"""
import re
import zipfile
import html

CN_FONT = '仿宋_GB2312'
CN_FALLBACK = '仿宋'
EN_FONT = 'Times New Roman'
HEI = '黑体'
MONO = 'Consolas'

# 正文字号（半点：小四 = 12pt = 24）
SZ_BODY = '24'
SZ_TABLE = '21'      # 五号 10.5pt
SZ_CODE = '18'       # 小五 9pt

# 把报告里用到的符号转成纯文字，避免出现 emoji
SYMBOL_MAP = [
    ('✅', '通过'), ('❌', '未通过'), ('⚠️', '注意'), ('⚠', '注意'),
    ('★', '＊'), ('☆', '＊'), ('→', '→'), ('←', '←'),
    ('·', '·'), ('—', '——'),
]


def clean_symbols(s):
    for a, b in SYMBOL_MAP:
        s = s.replace(a, b)
    return s


def esc(s):
    return html.escape(clean_symbols(s), quote=False)


def _rpr(bold=False, font=None, sz=None, mono=False, color=None, italic=False):
    parts = []
    if mono:
        parts.append('<w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s" w:cs="%s"/>'
                     % (MONO, MONO, MONO, MONO))
    elif font:
        parts.append('<w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s" w:cs="%s"/>'
                     % (EN_FONT if font == 'cn' else font, EN_FONT if font == 'cn' else font,
                        CN_FONT if font == 'cn' else font, CN_FALLBACK if font == 'cn' else font))
    if bold:
        parts.append('<w:b/><w:bCs/>')
    if italic:
        parts.append('<w:i/>')
    if color:
        parts.append('<w:color w:val="%s"/>' % color)
    if sz:
        parts.append('<w:sz w:val="%s"/><w:szCs w:val="%s"/>' % (sz, sz))
    return '<w:rPr>%s</w:rPr>' % ''.join(parts) if parts else ''


def runs(text, base_font='cn', base_sz=SZ_BODY):
    """处理 **粗体** 与 `代码`，其余按正文字体输出"""
    out = []
    parts = re.split(r'(\*\*[^*]+\*\*|`[^`]+`)', text)
    for p in parts:
        if not p:
            continue
        if p.startswith('**') and p.endswith('**'):
            out.append('<w:r>%s<w:t xml:space="preserve">%s</w:t></w:r>'
                       % (_rpr(bold=True, font=base_font, sz=base_sz), esc(p[2:-2])))
        elif p.startswith('`') and p.endswith('`'):
            out.append('<w:r><w:rPr><w:rFonts w:ascii="%s" w:hAnsi="%s"/>'
                       '<w:sz w:val="%s"/><w:szCs w:val="%s"/>'
                       '<w:shd w:val="clear" w:fill="F2F2F2"/></w:rPr>'
                       '<w:t xml:space="preserve">%s</w:t></w:r>'
                       % (MONO, MONO, SZ_CODE, SZ_CODE, esc(p[1:-1])))
        else:
            out.append('<w:r>%s<w:t xml:space="preserve">%s</w:t></w:r>'
                       % (_rpr(font=base_font, sz=base_sz), esc(p)))
    return ''.join(out)


def para(text, indent=True, font='cn', sz=SZ_BODY, first_line=None, spacing=None):
    ind = first_line if first_line is not None else ('<w:ind w:firstLineChars="200"/>' if indent else '')
    sp = spacing or '<w:spacing w:line="360" w:lineRule="auto" w:after="60"/>'
    return ('<w:p><w:pPr>%s%s</w:pPr>%s</w:p>'
            % (ind, sp, runs(text, font, sz)))


def heading(text, lvl):
    # 标题不缩进，黑体，按级别定字号
    sizes = {1: '36', 2: '32', 3: '28', 4: '24', 5: '24', 6: '24'}
    sz = sizes.get(lvl, '24')
    before = {1: '320', 2: '260', 3: '200', 4: '160'}.get(lvl, '160')
    return ('<w:p><w:pPr><w:keepNext/><w:spacing w:before="%s" w:after="120" w:line="360" w:lineRule="auto"/>'
            '<w:outlineLvl w:val="%d"/></w:pPr>'
            '<w:r>%s<w:t xml:space="preserve">%s</w:t></w:r></w:p>'
            % (before, min(lvl, 6) - 1,
               _rpr(bold=True, font=HEI, sz=sz), esc(text)))


def code_block(lines):
    out = []
    for idx, l in enumerate(lines):
        first = '<w:spacing w:before="40" w:after="0" w:line="240" w:lineRule="auto"/>' if idx == 0 else \
                '<w:spacing w:before="0" w:after="0" w:line="240" w:lineRule="auto"/>'
        last = '<w:spacing w:after="80"/>' if idx == len(lines) - 1 else ''
        out.append('<w:p><w:pPr><w:shd w:val="clear" w:fill="F5F5F5"/>%s%s'
                   '<w:ind w:left="240"/></w:pPr>'
                   '<w:r><w:rPr><w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s"/>'
                   '<w:sz w:val="%s"/><w:szCs w:val="%s"/></w:rPr>'
                   '<w:t xml:space="preserve">%s</w:t></w:r></w:p>'
                   % (first, last, MONO, MONO, MONO, SZ_CODE, SZ_CODE,
                      esc(l) if l.strip() else ' '))
    return ''.join(out)


def table(rows):
    if not rows:
        return ''
    cols = max(len(r) for r in rows)
    # 依据列数分配列宽（总宽约 9026 twips），首列窄一些更耐看
    total = 9000
    if cols <= 1:
        widths = [total]
    elif cols == 2:
        widths = [int(total * 0.34), total - int(total * 0.34)]
    else:
        w0 = int(total * 0.22)
        rest = (total - w0) // (cols - 1)
        widths = [w0] + [rest] * (cols - 1)
        widths[-1] = total - sum(widths[:-1])

    xml = ['<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/>'
           '<w:tblW w:w="%d" w:type="dxa"/>'
           '<w:jc w:val="center"/>'
           '<w:tblBorders>'
           '<w:top w:val="single" w:sz="6" w:color="808080"/>'
           '<w:left w:val="single" w:sz="6" w:color="808080"/>'
           '<w:bottom w:val="single" w:sz="6" w:color="808080"/>'
           '<w:right w:val="single" w:sz="6" w:color="808080"/>'
           '<w:insideH w:val="single" w:sz="4" w:color="A6A6A6"/>'
           '<w:insideV w:val="single" w:sz="4" w:color="A6A6A6"/>'
           '</w:tblBorders>'
           '<w:tblCellMar><w:top w:w="60" w:type="dxa"/><w:left w:w="90" w:type="dxa"/>'
           '<w:bottom w:w="60" w:type="dxa"/><w:right w:w="90" w:type="dxa"/></w:tblCellMar>'
           '</w:tblPr>' % total]
    xml.append('<w:tblGrid>%s</w:tblGrid>' % ''.join('<w:gridCol w:w="%d"/>' % w for w in widths))

    for i, r in enumerate(rows):
        header = (i == 0)
        xml.append('<w:tr><w:trPr>%s</w:trPr>' % ('<w:tblHeader/>' if header else ''))
        for c in range(cols):
            cell = r[c] if c < len(r) else ''
            body = runs(cell, base_font=HEI if header else 'cn', base_sz=SZ_TABLE)
            if header and not cell.strip():
                body = ''
            shade = '<w:shd w:val="clear" w:fill="EDEDED"/>' if header else ''
            jc = '<w:jc w:val="center"/>' if header else ''
            xml.append('<w:tc><w:tcPr><w:tcW w:w="%d" w:type="dxa"/>%s'
                       '<w:vAlign w:val="center"/></w:tcPr>'
                       '<w:p><w:pPr>%s<w:spacing w:line="280" w:lineRule="auto" '
                       'w:before="20" w:after="20"/></w:pPr>%s</w:p></w:tc>'
                       % (widths[c], shade, jc, body))
        xml.append('</w:tr>')
    xml.append('</w:tbl>')
    # 表格后留一点空隙
    xml.append('<w:p><w:pPr><w:spacing w:after="120"/></w:pPr></w:p>')
    return ''.join(xml)


def convert(md):
    md = clean_symbols(md)
    lines = md.split('\n')
    body = []
    i = 0
    while i < len(lines):
        l = lines[i]
        s = l.strip()

        # 代码块
        if s.startswith('```'):
            i += 1
            buf = []
            while i < len(lines) and not lines[i].strip().startswith('```'):
                buf.append(lines[i].rstrip())
                i += 1
            i += 1
            body.append(code_block(buf))
            continue

        # 表格
        if s.startswith('|') and i + 1 < len(lines) and re.match(r'^\|[\s\-:|]+\|$', lines[i + 1].strip()):
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                row = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                if not re.match(r'^[\s\-:]+$', ''.join(row)):
                    rows.append(row)
                i += 1
            body.append(table(rows))
            continue

        # 标题
        m = re.match(r'^(#{1,6})\s+(.*)$', s)
        if m:
            body.append(heading(m.group(2).strip(), len(m.group(1))))
            i += 1
            continue

        # 引用
        if s.startswith('> '):
            body.append(para(s[2:].strip(), indent=False, sz='21'))
            i += 1
            continue

        # 无序列表（不缩进，用 ·）
        if re.match(r'^[-*]\s+', s):
            txt = re.sub(r'^[-*]\s+', '', s)
            body.append(para('· ' + txt, indent=False))
            i += 1
            continue

        # 有序列表
        m = re.match(r'^(\d+)\.\s+(.*)$', s)
        if m:
            body.append(para(m.group(1) + '. ' + m.group(2), indent=False))
            i += 1
            continue

        # 分隔线
        if s.startswith('---'):
            body.append('<w:p><w:pPr><w:pBdr><w:bottom w:val="single" w:sz="6" w:color="BFBFBF"/>'
                        '</w:pBdr><w:spacing w:after="120"/></w:pPr></w:p>')
            i += 1
            continue

        if not s:
            i += 1
            continue

        body.append(para(s))
        i += 1

    doc = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
           '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
           '<w:body>%s'
           '<w:sectPr>'
           '<w:pgSz w:w="11906" w:h="16838"/>'
           '<w:pgMar w:top="1440" w:right="1418" w:bottom="1440" w:left="1587" '
           'w:header="851" w:footer="992" w:gutter="0"/>'
           '<w:cols w:space="425"/>'
           '<w:docGrid w:type="lines" w:linePitch="312"/>'
           '</w:sectPr>'
           '</w:body></w:document>' % ''.join(body))
    return doc


CT = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
</Types>'''

RELS = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>'''

DOCRELS = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>'''


def _font_rpr(font, sz):
    return ('<w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s" w:cs="%s"/>'
            '<w:sz w:val="%s"/><w:szCs w:val="%s"/>'
            % (EN_FONT, EN_FONT, font, CN_FALLBACK, sz, sz))


def styles():
    def h(n, sz):
        return ('<w:style w:type="paragraph" w:styleId="Heading%d"><w:name w:val="heading %d"/>'
                '<w:basedOn w:val="Normal"/><w:next w:val="Normal"/>'
                '<w:pPr><w:keepNext/><w:keepLines/>'
                '<w:spacing w:before="240" w:after="120" w:line="360" w:lineRule="auto"/>'
                '<w:outlineLvl w:val="%d"/></w:pPr>'
                '<w:rPr><w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s" w:cs="%s"/>'
                '<w:b/><w:bCs/><w:sz w:val="%s"/><w:szCs w:val="%s"/></w:rPr></w:style>'
                % (n, n, n - 1, HEI, HEI, HEI, HEI, sz, sz))

    return ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            '<w:docDefaults><w:rPrDefault><w:rPr>'
            + _font_rpr(CN_FONT, SZ_BODY) +
            '</w:rPr></w:rPrDefault>'
            '<w:pPrDefault><w:pPr>'
            '<w:spacing w:line="360" w:lineRule="auto" w:after="60"/>'
            '</w:pPr></w:pPrDefault></w:docDefaults>'
            '<w:style w:type="paragraph" w:default="1" w:styleId="Normal">'
            '<w:name w:val="Normal"/>'
            '<w:pPr><w:spacing w:line="360" w:lineRule="auto" w:after="60"/></w:pPr>'
            '<w:rPr>' + _font_rpr(CN_FONT, SZ_BODY) + '</w:rPr></w:style>'
            + h(1, '36') + h(2, '32') + h(3, '28') + h(4, '24') + h(5, '24') + h(6, '24') +
            '<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/>'
            '<w:tblPr><w:tblBorders>'
            '<w:top w:val="single" w:sz="6" w:color="808080"/>'
            '<w:left w:val="single" w:sz="6" w:color="808080"/>'
            '<w:bottom w:val="single" w:sz="6" w:color="808080"/>'
            '<w:right w:val="single" w:sz="6" w:color="808080"/>'
            '<w:insideH w:val="single" w:sz="4" w:color="A6A6A6"/>'
            '<w:insideV w:val="single" w:sz="4" w:color="A6A6A6"/>'
            '</w:tblBorders></w:tblPr>'
            '<w:tcPr><w:vAlign w:val="center"/></w:tcPr>'
            '<w:rPr>' + _font_rpr(CN_FONT, SZ_TABLE) + '</w:rPr></w:style>'
            '</w:styles>')


if __name__ == '__main__':
    import sys
    src, dst = sys.argv[1], sys.argv[2]
    md = open(src, encoding='utf-8').read()
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml', CT)
        z.writestr('_rels/.rels', RELS)
        z.writestr('word/_rels/document.xml.rels', DOCRELS)
        z.writestr('word/styles.xml', styles())
        z.writestr('word/document.xml', convert(md))
    print('已生成:', dst)

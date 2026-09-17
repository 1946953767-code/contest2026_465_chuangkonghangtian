# 生成思澈 LCD 用的 16x16 中文点阵字库（仅包含界面需要的字）
from PIL import Image, ImageDraw, ImageFont

CHARS = (
    "状态曲线速度姿态火箭卫星连接阶段"
    "待命确认点火飞行结束灭火高度温度气压时间"
    "记录搜索俯仰滚转偏航加速倾斜"
    "任务按键新建停止正常无有中断开"
    "显示数据页轴图标值前秒定位已"
    "地基站北雷达成方位距离相对坐标"
)

FONT_PATH = r"C:\Windows\Fonts\msyh.ttc"
SIZE = 16


def render(ch):
    img = Image.new("1", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT_PATH, SIZE)
    bbox = d.textbbox((0, 0), ch, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (SIZE - w) // 2 - bbox[0]
    y = (SIZE - h) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=1)
    rows = []
    for j in range(SIZE):
        row = 0
        for i in range(SIZE):
            if img.getpixel((i, j)):
                row |= (1 << (SIZE - 1 - i))
        rows.append(row)
    return rows


lines = []
lines.append("/* Auto-generated 16x16 CJK bitmap font for SF32 LCD (UI subset). */")
lines.append("typedef struct { unsigned char utf8[3]; unsigned char bmp[32]; } cn_glyph_t;")
lines.append("static const cn_glyph_t cn_font[] = {")
seen = set()
for ch in CHARS:
    if ch in seen:
        continue
    seen.add(ch)
    rows = render(ch)
    b = ch.encode("utf-8")
    assert len(b) == 3, ch
    bmp = []
    for r in rows:
        bmp.append((r >> 8) & 0xFF)
        bmp.append(r & 0xFF)
    hexs = ",".join("0x%02X" % v for v in bmp)
    lines.append("    {{0x%02X,0x%02X,0x%02X},{%s}}," % (b[0], b[1], b[2], hexs))
lines.append("};")
lines.append("#define CN_FONT_COUNT (sizeof(cn_font)/sizeof(cn_font[0]))")

with open(r"C:\Users\HUAWEI\Desktop\cn_font.h", "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines) + "\n")

print("GLYPHS", len(seen))
print("BYTES", len(seen) * 35)

"""被动读取 ESP32(COM7) 串口输出，不发送任何字节。

用法: python scripts/com7_probe.py [秒数]
"""
import sys
import time

import serial
from serial.tools import list_ports

PORT = "COM7"
BAUD = 115200
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

print("== 可见串口 ==")
for p in list_ports.comports():
    print(f"  {p.device}  {p.description}  hwid={p.hwid}")

print(f"\n== 打开 {PORT} @ {BAUD} ==")
s = serial.Serial(PORT, BAUD, timeout=0.3)
# ESP32 自动复位电路: RTS->EN, DTR->IO0。两者拉低=正常运行，避免误触发下载模式
s.setRTS(False)
s.setDTR(False)
time.sleep(1.0)
s.reset_input_buffer()
print("已连接，开始被动采集 %.0f 秒...\n" % SECONDS)

t0 = time.time()
buf = bytearray()
while time.time() - t0 < SECONDS:
    d = s.read(4096)
    if d:
        buf += d

s.close()

zero = sum(1 for b in buf if b == 0)
text = buf.decode("utf-8", "replace")

# 留存原始证据（skill 的"先取证"原则：每次改动都保留可复现的串口日志）
import os
os.makedirs("logs", exist_ok=True)
out = os.path.join("logs", time.strftime("com7_capture_%Y%m%d_%H%M%S.txt"))
with open(out, "w", encoding="utf-8") as f:
    f.write(text)
print("[日志已保存] %s" % out)

print("== 原始输出 (%d 字节, 异常 0x00 字节 %d) ==" % (len(buf), zero))
print(text)
print("== 统计 ==")
print("总字节", len(buf), "| 行数", text.count("\n"), "| 0x00 字节", zero)

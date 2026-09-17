"""在**保持串口已打开**的状态下软重启 ESP32(COM7)，完整抓取开机自检输出。

背景：`mpremote connect COM7 reset` 之后再去开串口，开机前几十行（TFT/IMU/气压计/GPS/LoRa
自检）已经打完了，抓不到。所以这里先开串口，再用 Ctrl-C 打断当前脚本 + Ctrl-D 触发
MicroPython 软重启，这样从第一行开始就在收。

用法: python scripts/esp32_reboot_capture.py [秒数]
"""
import os
import sys
import time

import serial

PORT = "COM7"
BAUD = 115200
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0

s = serial.Serial(PORT, BAUD, timeout=0.3)
s.setRTS(False)
s.setDTR(False)
time.sleep(0.5)
s.reset_input_buffer()

# Ctrl-C 打断正在运行的 main 脚本，回到 REPL
s.write(b"\x03")
time.sleep(0.4)
s.write(b"\x03")
time.sleep(0.4)
# Ctrl-D = 软重启，重新执行 boot.py / main.py
s.write(b"\x04")

t0 = time.time()
buf = bytearray()
while time.time() - t0 < SECONDS:
    d = s.read(4096)
    if d:
        buf += d
s.close()

text = buf.decode("utf-8", "replace")
zero = sum(1 for b in buf if b == 0)

os.makedirs("logs", exist_ok=True)
out = os.path.join("logs", time.strftime("esp32_boot_%Y%m%d_%H%M%S.txt"))
with open(out, "w", encoding="utf-8") as f:
    f.write(text)

print("== 开机输出 (%d 字节, 0x00 异常 %d) ==" % (len(buf), zero))
for line in text.splitlines():
    if line.strip():
        print("  " + line.strip())
print("\n[日志已保存] %s" % out)

# -*- coding: utf-8 -*-
"""A28 链路质量量化：同时抓 ESP32 发送计数与地面站接收计数，算吞吐/丢包。"""
import serial, time, sys, re
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 90

esp = serial.Serial('COM7', 115200, timeout=0.2)
esp.setDTR(False); esp.setRTS(False); time.sleep(0.2); esp.reset_input_buffer()
sf = serial.Serial('COM9', 1000000, timeout=0.2)
sf.setDTR(False); sf.setRTS(False); time.sleep(0.2)
sf.setRTS(True); time.sleep(0.25); sf.setRTS(False); sf.setDTR(False)
sf.reset_input_buffer()

print("=== 采集 %d 秒 ===" % DUR, flush=True)
t0 = time.time()
eb = b""; sb = b""; samples = []
while time.time() - t0 < DUR:
    d = esp.read(65536)
    if d: eb += d
    d = sf.read(65536)
    if d:
        sb += d
        for ln in d.decode('utf-8', 'replace').splitlines():
            m = re.search(r'LoRa packets=(\d+) probe_reply=(\d+) rocket_link=(-?\d+)', ln)
            if m:
                samples.append((round(time.time() - t0, 1), int(m.group(1)), int(m.group(2)), int(m.group(3))))
    time.sleep(0.01)
esp.close(); sf.close()

et = eb.decode('utf-8', 'replace'); st = sb.decode('utf-8', 'replace')

# ★ 抓 ESP32 的关键提示行（全双工自动兜底等）
for ln in et.splitlines():
    if ('★' in ln) or ('全双工' in ln) or ('FULL-DUPLEX' in ln) or ('MD0' in ln):
        print("[ESP32 提示] " + ln.strip())

stx = [int(x) for x in re.findall(r'STAT tx=(\d+)', et)]
scmd = [int(x) for x in re.findall(r'cmd=(\d+)', et)]
sjunk = [int(x) for x in re.findall(r'junk=(\d+)', et)]
slora = [int(x) for x in re.findall(r'lora=(\d+)', et)]

print("\n================ 链路质量 ================")
if stx:
    print("ESP32 发送 tx : %d -> %d   共 %d 包，%.2f 包/秒"
          % (stx[0], stx[-1], stx[-1]-stx[0], (stx[-1]-stx[0])/DUR))
if scmd:
    print("ESP32 收命令  : %d -> %d   共 %d 条" % (scmd[0], scmd[-1], scmd[-1]-scmd[0]))
if sjunk:
    print("ESP32 噪声    : %d -> %d   %s" % (sjunk[0], sjunk[-1], "未增长 ✅" if sjunk[-1]==sjunk[0] or sjunk[-1]-sjunk[0]<=2 else "⚠ 仍在增长"))
if slora:
    lv = ("始终为1 ✅" if min(slora) == 1 else ("曾转为1 ✅" if max(slora) == 1 else "始终为0 ⚠"))
    print("ESP32 lora 标志: %s   序列=%s" % (lv, slora))
if scmd:
    print("ESP32 cmd 序列 : %s" % scmd)

if len(samples) >= 2:
    a, b = samples[0], samples[-1]
    dpk = b[1]-a[1]; dt = b[0]-a[0]
    print("地面站 收包     : %d -> %d   共 %d 包，%.2f 包/秒" % (a[1], b[1], dpk, dpk/dt if dt else 0))
    if stx:
        sent = stx[-1]-stx[0]
        print("→ 丢包率      : %.1f%%  (发 %d / 收 %d)" % (max(0, (sent-dpk))*100.0/max(1,sent), sent, dpk))
    print("地面站 probe_reply: %d -> %d    rocket_link: %d -> %d" % (a[2], b[2], a[3], b[3]))

njson = len(re.findall(r'\{"t":', st))
print("地面站收到遥测 JSON 行: %d" % njson)
nack = len(re.findall(r'ESP32 ACK', st))
print("地面站收到 ESP32 ACK  : %d" % nack)
print("==========================================")

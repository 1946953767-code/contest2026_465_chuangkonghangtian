# -*- coding: utf-8 -*-
"""A28 链路决定性测试：同时监视 ESP32(COM7) 与 地面站(COM9)。
判据：ESP32 cmd 递增 = 地面站->ESP32 通；地面站出现 JSON/ESP32 ACK = ESP32->地面站 通。"""
import serial, time, sys, re
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 80

# --- 先复位 ESP32（mpremote 已在上层调用），这里只负责打开串口 ---
esp = serial.Serial('COM7', 115200, timeout=0.2)
esp.setDTR(False); esp.setRTS(False); time.sleep(0.2); esp.reset_input_buffer()

# --- 复位地面站：RTS 脉冲，然后必须放开否则 SoC 被按住 ---
sf = serial.Serial('COM9', 1000000, timeout=0.2)
sf.setDTR(False); sf.setRTS(False); time.sleep(0.2)
sf.setRTS(True); time.sleep(0.25); sf.setRTS(False)
sf.setDTR(False); sf.reset_input_buffer()

print("=== 复位完成，采集 %d 秒 ===" % DUR, flush=True)
eb = b''; sb = b''
t0 = time.time()
while time.time() - t0 < DUR:
    d = esp.read(16384)
    if d:
        eb += d
        for ln in d.decode('utf-8', 'replace').splitlines():
            if ln.strip():
                print("[ESP32] " + ln.strip()[:160], flush=True)
    d = sf.read(16384)
    if d:
        sb += d
        for ln in d.decode('utf-8', 'replace').splitlines():
            if ln.strip():
                print("[地面站 ] " + ln.strip()[:160], flush=True)
    time.sleep(0.02)
esp.close(); sf.close()

et = eb.decode('utf-8', 'replace')
st = sb.decode('utf-8', 'replace')

print("\n================ 判定 ================", flush=True)
m = re.findall(r'cmd=(\d+)', et)
if m:
    print("ESP32 cmd: %s .. %s  -> 地面站->ESP32 %s" % (m[0], m[-1], "通 ✅" if int(m[-1]) > 0 else "断 ❌"))
else:
    print("ESP32 未打印 cmd= -> 地面站->ESP32 断 ❌")

tx = re.findall(r'tx=(\d+)', et)
if tx: print("ESP32 tx 计数: %s .. %s" % (tx[0], tx[-1]))

njson = len(re.findall(r'\{"p":', st))
nack = len(re.findall(r'ESP32 ACK', st))
print("地面站收到 JSON 帧: %d   ESP32 ACK: %d" % (njson, nack))
print("ESP32->地面站 %s" % ("通 ✅✅✅" if (njson or nack) else "断 ❌"))

# 地面站是否认出 LoRa 串口
for kw in ('LoRa UART2', 'lora', 'LoRa'):
    for ln in st.splitlines():
        if kw in ln:
            print("地面站日志: " + ln.strip()[:140]); break
    else:
        continue
    break

# 地面站是否有任何"疑似空中数据"的字符（非日志行）
odd = [l for l in st.splitlines() if re.search(r'[{}]', l) and 'pid' not in l]
print("地面站含花括号的行数: %d" % len(odd))
for l in odd[:5]:
    print("   > " + l.strip()[:140])
print("======================================", flush=True)

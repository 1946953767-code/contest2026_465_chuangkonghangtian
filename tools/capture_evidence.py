import serial
import time
import re

OUT = r'C:\Users\HUAWEI\Desktop\openvela_ai_contest_submission\evidence\串口日志.txt'

s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False)
s.setDTR(False)
time.sleep(2)
s.reset_input_buffer()

lines = []
t0 = time.time()
while time.time() - t0 < 40:
    d = s.read(8192)
    if d:
        lines.append(d.decode('utf-8', 'replace'))

text = ''.join(lines)
with open(OUT, 'w', encoding='utf-8') as f:
    f.write('=== 思澈 SF32LB52 地面端串口日志（COM9, 1000000bps）===\n')
    f.write('采集时间: ' + time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
    f.write('说明: RTS/DTR 已拉低，避免把 SoC 压在复位状态\n')
    f.write('=' * 60 + '\n\n')
    f.write(text)

print('已保存:', OUT)
print('字节数:', len(text))
print('遥测行数:', text.count('"t":'))
print('雷达行数:', text.count('"gs_lat"'))
print('LoRa 计数行:', len(re.findall(r'LoRa packets=', text)))
print()
print('--- 样例 ---')
sample = [l for l in text.split('\n') if l.startswith('{"t"')]
if sample:
    print(sample[-1][:300])
radar = [l for l in text.split('\n') if l.startswith('{"gs_rx"')]
if radar:
    print(radar[-1][:300])
s.close()

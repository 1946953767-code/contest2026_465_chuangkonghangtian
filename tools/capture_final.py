import serial
import time
import re

s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False)
s.setDTR(False)
time.sleep(2)
s.reset_input_buffer()

t0 = time.time()
b = b''
while time.time() - t0 < 30:
    d = s.read(8192)
    if d:
        b += d
t = b.decode('utf-8', 'replace')

out = r'C:\Users\HUAWEI\Desktop\openvela_ai_contest_submission\evidence\串口日志.txt'
with open(out, 'w', encoding='utf-8') as f:
    f.write('=== 思澈 SF32LB52 地面端串口日志（COM9 @ 1000000 bps） ===\n')
    f.write('采集时间: ' + time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
    f.write('采集时长: 30 秒\n')
    f.write('说明: 串口打开后已显式拉低 RTS/DTR，避免 SoC 被压在复位状态\n')
    f.write('=' * 64 + '\n\n')
    f.write(t)

print('已保存证据:', out)
print('总字节:', len(b))
print('')
print('遥测 JSON 行 :', t.count('"t":'))
print('雷达 JSON 行 :', t.count('"gs_lat"'))
print('LoRa 包计数  :', re.findall(r'LoRa packets=(\d+)', t)[:6])
print('ESP32 ACK    :', re.findall(r'ESP32 ACK (\w+)', t)[:3])
print('')
js = [l for l in t.split('\n') if l.startswith('{"t"')]
if js:
    print('最后遥测:', js[-1][:320])
rd = [l for l in t.split('\n') if l.startswith('{"gs_rx"')]
if rd:
    print('最后雷达:', rd[-1][:320])
s.close()

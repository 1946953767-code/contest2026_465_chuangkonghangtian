import serial
import time
import re

s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False)
s.setDTR(False)
time.sleep(1)

# 停止旧实例并重启，全程不关闭端口
s.write(b'\x03\x03')
time.sleep(2)
s.reset_input_buffer()
s.write(b'uart2hwtest\r\n')
time.sleep(5)
banner = s.read(4000).decode('utf-8', 'replace')
print('--- 启动日志 ---')
print(banner)

s.reset_input_buffer()
t0 = time.time()
b = b''
while time.time() - t0 < 25:
    d = s.read(8192)
    if d:
        b += d
t = b.decode('utf-8', 'replace')

print('--- 25 秒运行统计 ---')
print('遥测行数 :', t.count('"t":'))
print('雷达行数 :', t.count('"gs_lat"'))
print('LoRa 计数:', re.findall(r'LoRa packets=(\d+)', t)[:5])
print('ESP32 ACK:', re.findall(r'ESP32 ACK (\w+)', t)[:3])
print('SAT dev  :', re.findall(r'SAT dev -> (\S+)', t)[:3])
print('SAT baud :', re.findall(r'SAT baud -> (\d+)', t)[:5])

lines = [l for l in t.split('\n') if l.startswith('{"t"')]
if lines:
    print('\n最后一条遥测:')
    print(lines[-1][:300])
radar = [l for l in t.split('\n') if l.startswith('{"gs_rx"')]
if radar:
    print('\n最后一条雷达:')
    print(radar[-1][:300])
s.close()

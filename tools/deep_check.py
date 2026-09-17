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
while time.time() - t0 < 45:
    d = s.read(8192)
    if d:
        b += d
t = b.decode('utf-8', 'replace')

print('=== 45 秒统计 ===')
print('总字节      :', len(b))
print('遥测 JSON   :', t.count('"t":'))
print('雷达 JSON   :', t.count('"gs_lat"'))
print('LoRa 计数行 :', len(re.findall(r'LoRa packets=(\d+)', t)))
print('包计数序列  :', re.findall(r'LoRa packets=(\d+)', t)[:8])
print('ESP32 ACK   :', re.findall(r'ESP32 ACK (\w+)', t)[:5])
print('')
print('=== 全部非 JSON 行（最多 30 行）===')
n = 0
for l in t.split('\n'):
    l = l.strip()
    if not l or l.startswith('{"'):
        continue
    print(' ', l[:150])
    n += 1
    if n >= 30:
        break
print('')
print('=== 最后 600 字符 ===')
print(t[-600:])
s.close()

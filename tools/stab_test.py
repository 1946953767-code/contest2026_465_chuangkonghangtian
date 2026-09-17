import serial
import time

s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False)
s.setDTR(False)
time.sleep(2)

print('观察 90 秒…')
t0 = time.time()
last = 0
total = 0
zero = 0

while time.time() - t0 < 90:
    d = s.read(8192)
    if d:
        total += len(d)
        zero += sum(1 for b in d if b == 0)
    el = time.time() - t0
    if el - last >= 20:
        last = el
        print('  t=%3ds  bytes=%6d  zero=%d' % (int(el), total, zero))

t = ''
print('--- 结果 ---')
print('总字节:', total)
print('零字节:', zero)
print('判定:', '卡死' if (total == 0 or zero == total) else '正常')
s.close()

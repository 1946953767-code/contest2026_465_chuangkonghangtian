from machine import Pin, SoftI2C
import time

# 工程文档出现过的所有 GPIO 候选
cands = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,
         26,33,34,35,36,37,38,39,40,41,42,44,45,46,47,48]

found = []
for scl in cands:
    for sda in cands:
        if sda == scl:
            continue
        try:
            b = SoftI2C(scl=Pin(scl), sda=Pin(sda), freq=100000, timeout=20000)
            devs = b.scan()
        except Exception:
            continue
        if devs:
            # 只打印包含 0x76/0x77（BME/BMP）或任何设备的总线
            found.append((scl, sda, devs))
        # 每次尝试后把引脚复原，避免残留
        try:
            p1 = Pin(scl, Pin.IN); p2 = Pin(sda, Pin.IN)
        except Exception:
            pass
        time.sleep_ms(1)

print("FOUND:", found)
if not found:
    print("NO_I2C_DEVICES_ON_ANY_CANDIDATE_BUS")
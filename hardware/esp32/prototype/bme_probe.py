from machine import Pin, SoftI2C
import time

buses = [(12, 13), (9, 8), (16, 17), (21, 10)]
for scl, sda in buses:
    try:
        b = SoftI2C(scl=Pin(scl), sda=Pin(sda), freq=100000)
        devs = b.scan()
        print("BUS", scl, sda, [hex(x) for x in devs])
        for a in (0x76, 0x77):
            if a in devs:
                try:
                    print("  chip", hex(a), "ID", hex(b.readfrom_mem(a, 0xD0, 1)[0]))
                except Exception as e:
                    print("  chip", hex(a), "read err", e)
    except Exception as e:
        print("BUS", scl, sda, "ERROR", e)
    time.sleep_ms(100)

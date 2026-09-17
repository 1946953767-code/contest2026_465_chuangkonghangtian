from machine import Pin, SoftI2C
import time

for scl, sda in ((3, 9), (9, 3)):
    try:
        bus = SoftI2C(scl=Pin(scl), sda=Pin(sda), freq=100000)
        devs = bus.scan()
        print("SCL", scl, "SDA", sda, [hex(x) for x in devs])
        for addr in (0x76, 0x77):
            if addr in devs:
                print("BME", hex(addr), "ID", hex(bus.readfrom_mem(addr, 0xD0, 1)[0]))
    except Exception as e:
        print("SCL", scl, "SDA", sda, "ERR", e)
    time.sleep_ms(100)

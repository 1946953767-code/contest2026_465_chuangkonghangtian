# 测卡的"每块写入耗时"——判断瓶颈是 SPI 速度还是卡本身
import machine
import os
import time
import sdcard

SD_CS, SD_SCK, SD_MOSI, SD_MISO = 14, 21, 47, 4

for baud in (400000, 100000):
    spi = machine.SoftSPI(baudrate=baud, polarity=0, phase=0,
                          sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                          miso=machine.Pin(SD_MISO, machine.Pin.IN, machine.Pin.PULL_UP))
    sd = sdcard.SDCard(spi, machine.Pin(SD_CS, machine.Pin.OUT), baud)
    try:
        os.mount(sd, "/sd")
    except Exception as e:
        print("挂载失败:", e)
        continue

    print("=== SPI %d Hz ===" % baud)
    for size, label in ((512, "1 块"), (1024, "2 块"), (4096, "8 块")):
        try:
            t0 = time.ticks_ms()
            with open("/sd/sp.bin", "w") as f:
                f.write(b"x" * size)
            dt = time.ticks_diff(time.ticks_ms(), t0)
            per = dt * 1024 // size
            print("  写 %-5s (%5d B)  %5d ms   折合每 KB %d ms"
                  % (label, size, dt, per))
        except Exception as e:
            print("  写 %s 失败: %s" % (label, e))
    try:
        os.remove("/sd/sp.bin")
    except Exception:
        pass
    try:
        os.umount("/sd")
    except Exception:
        pass
    time.sleep_ms(200)

print()
print("判读：")
print("  每块耗时 ≈ 100~250ms  → 卡本身慢（老卡/小容量卡），换 Class10 卡能显著改善")
print("  每块耗时随 SPI 速率明显变化 → 瓶颈在 SPI，提速有用")

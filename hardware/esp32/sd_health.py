# SD 卡体检：逐条排查"卡为什么认不出来"，最后给出结论
# 覆盖：引脚电气 / 裸 CMD0 / 完整初始化 / 挂载 / 读写 / 反复插拔稳定性
import machine
import os
import time
import sdcard

SD_CS, SD_SCK, SD_MOSI, SD_MISO = 14, 21, 47, 4
PINS = [('CS', SD_CS), ('SCK', SD_SCK), ('MOSI', SD_MOSI), ('MISO', SD_MISO)]


def release_pins():
    for _, n in PINS:
        try:
            machine.Pin(n, machine.Pin.IN)
        except Exception:
            pass
    time.sleep_ms(80)


print("=" * 52)
print("SD 卡体检")
print("=" * 52)

# ---- 1) 引脚电气状态 ----
print("\n[1] 引脚电气状态（可被拉高拉低才算正常）")
pin_bad = []
for name, num in PINS:
    try:
        p = machine.Pin(num, machine.Pin.IN, machine.Pin.PULL_UP)
        time.sleep_ms(15)
        up = p.value()
        p = machine.Pin(num, machine.Pin.IN, machine.Pin.PULL_DOWN)
        time.sleep_ms(15)
        dn = p.value()
        if up == 1 and dn == 0:
            verdict = "正常"
        elif up == 1 and dn == 1:
            verdict = "恒为高（接了上拉/接错/VCC）"
            pin_bad.append(name)
        else:
            verdict = "恒为低（短路到 GND/接错）"
            pin_bad.append(name)
        print("   GPIO%-3d %-5s 上拉=%d 下拉=%d  %s" % (num, name, up, dn, verdict))
    except Exception as e:
        print("   GPIO%-3d %-5s 读取失败: %s" % (num, name, e))
release_pins()

# ---- 2) 裸 CMD0 ----
print("\n[2] 裸 CMD0（卡是否在总线上应答）")
cmd0_ok = False
try:
    cs = machine.Pin(SD_CS, machine.Pin.OUT, value=1)
    spi = machine.SoftSPI(baudrate=200000, polarity=0, phase=0,
                          sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                          miso=machine.Pin(SD_MISO, machine.Pin.IN, machine.Pin.PULL_UP))
    cs(1)
    for _ in range(16):
        spi.write(b"\xff")
    cs(0)
    time.sleep_ms(5)
    spi.write(b"\x40\x00\x00\x00\x00\x95")
    r = 0xFF
    for _ in range(16):
        r = spi.read(1, 0xFF)[0]
        if r != 0xFF:
            break
    cs(1)
    spi.write(b"\xff")
    if r == 0x01:
        print("   R1=0x01  卡有应答，电气通路正常")
        cmd0_ok = True
    elif r == 0xFF:
        print("   R1=0xFF  无应答 —— 卡没接到总线上")
        print("            查：卡片是否插到底、卡座接触、VCC/GND、MISO 那一根线")
    else:
        print("   R1=0x%02X  异常响应" % r)
except Exception as e:
    print("   测试异常:", e)
release_pins()

# ---- 3) 完整初始化（多次，看稳定性）----
print("\n[3] 完整初始化（连续 5 次，看稳定性）")
ok_cnt = 0
last_err = ''
for i in range(5):
    try:
        t0 = time.ticks_ms()
        spi = machine.SoftSPI(baudrate=400000, polarity=0, phase=0,
                              sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                              miso=machine.Pin(SD_MISO, machine.Pin.IN, machine.Pin.PULL_UP))
        sd = sdcard.SDCard(spi, machine.Pin(SD_CS, machine.Pin.OUT), 400000)
        dt = time.ticks_diff(time.ticks_ms(), t0)
        print("   第%d次 成功  sectors=%d cdv=%d  用时 %d ms"
              % (i + 1, sd.sectors, sd.cdv, dt))
        ok_cnt += 1
        del sd
    except Exception as e:
        print("   第%d次 失败  %s" % (i + 1, e))
        last_err = str(e)
    release_pins()

# ---- 4) 挂载 + 读写 ----
print("\n[4] 挂载与读写")
if ok_cnt:
    try:
        spi = machine.SoftSPI(baudrate=400000, polarity=0, phase=0,
                              sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                              miso=machine.Pin(SD_MISO, machine.Pin.IN, machine.Pin.PULL_UP))
        sd = sdcard.SDCard(spi, machine.Pin(SD_CS, machine.Pin.OUT), 400000)
        try:
            os.mount(sd, "/sd")
            print("   挂载成功")
        except Exception as e:
            print("   挂载失败:", e)
            raise SystemExit
        print("   卡上文件:", os.listdir("/sd")[:8])
        t0 = time.ticks_ms()
        with open("/sd/_probe.csv", "w") as f:
            f.write("a,b\n1,2\n")
        print("   写测试文件 %d ms" % time.ticks_diff(time.ticks_ms(), t0))
        with open("/sd/_probe.csv") as f:
            print("   读回:", repr(f.read()))
        os.remove("/sd/_probe.csv")
        print("   删除 OK")
    except Exception as e:
        print("   读写异常:", e)

# ---- 结论 ----
print("\n" + "=" * 52)
print("结论")
print("=" * 52)
if pin_bad:
    print("  引脚异常: %s —— 先解决接线" % pin_bad)
elif not cmd0_ok:
    print("  卡在总线上无应答（CMD0 失败）")
    print("  按可能性排序：")
    print("   1. 卡片金手指氧化 → 拔出来用橡皮擦干净，重新插紧")
    print("   2. 卡座接触不良 → 插到底、轻按卡再上电")
    print("   3. 卡座 VCC 供电不足 → VCC-GND 就近并 100uF 电容")
    print("   4. 杜邦线/焊接接触 → 重点查 MISO(GPIO4) 这根")
    print("   5. 卡本身坏或不兼容 → 换一张卡（FAT32）最快")
elif ok_cnt == 0:
    print("  CMD0 有应答但初始化全部失败（%s）" % last_err)
    print("  → 卡能被选中但进不了就绪态，多为供电不足或卡状态异常")
    print("    1. 断电重启整块板子再试")
    print("    2. VCC 并 100uF 电容")
    print("    3. 换卡")
elif ok_cnt < 5:
    print("  初始化不稳定（%d/5 成功）—— 接触或供电余量不足" % ok_cnt)
    print("  → 优先重新插卡、就近加去耦电容")
else:
    print("  全部正常（5/5 初始化成功、挂载读写通过）")
    print("  → 若主程序仍失败，检查是否被其他外设干扰（TFT 用硬件 SPI2，SD 走 SoftSPI）")
print("=" * 52)

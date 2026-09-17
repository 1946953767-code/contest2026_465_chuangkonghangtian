# SD 卡功能自测：插卡后上电或 mpremote run 本脚本即可。
# 测什么：
#   1) 能否识别并挂载（SoftSPI @400kHz，硬件 SPI 留给 TFT）
#   2) 容量/扇区数
#   3) 写入 + 读回校验（20 行 CSV）
#   4) 单次写入耗时 —— 用来判断写卡会不会拖慢主循环、干扰 LoRa 时序
#   5) 清理测试文件
import machine
import os
import time
import sdcard

SD_CS, SD_SCK, SD_MOSI, SD_MISO = 14, 21, 47, 4
TEST = "/sd/sd_selftest.csv"

print("=== SD 自测开始 ===")

# --- 1) 挂载 ---
sd = None
try:
    spi = machine.SoftSPI(baudrate=400000, polarity=0, phase=0,
                          sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                          miso=machine.Pin(SD_MISO, machine.Pin.IN, machine.Pin.PULL_UP))
    sd = sdcard.SDCard(spi, machine.Pin(SD_CS, machine.Pin.OUT), 400000)
    print("1) 识别成功  扇区数 =", sd.sectors,
          " 容量约 %.2f GB" % (sd.sectors * 512 / 1024 / 1024 / 1024))
except Exception as e:
    print("1) 识别失败 ->", e)
    print("   排查：卡是否插紧 / 是否 FAT32 格式 / CS 是否 GPIO45 /")
    print("         供电是否够（SD 瞬时电流较大）/ 换一张卡试")
    raise SystemExit

try:
    os.mount(sd, "/sd")
    print("   挂载成功 -> /sd")
except Exception as e:
    print("2) 挂载失败 ->", e)
    print("   （若提示已挂载，可先 os.umount('/sd')）")
    raise SystemExit

# --- 2) 列目录 ---
try:
    files = os.listdir("/sd")
    print("2) 卡上文件 %d 个:" % len(files), files[:12])
except Exception as e:
    print("2) 列目录失败 ->", e)

# --- 3) 写入 + 计时 ---
rows = 20
header = "idx,time_ms,lat,lon,alt_m,temp_c,press_pa\n"
writes = []
try:
    t0 = time.ticks_ms()
    with open(TEST, "w") as f:
        f.write(header)
    print("3) 建文件耗时 %d ms" % time.ticks_diff(time.ticks_ms(), t0))

    for i in range(rows):
        line = "%d,%d,22.175900,113.073100,%.1f,%.1f,%.1f\n" % (
            i, time.ticks_ms(), 100.0 + i, 26.0 + i * 0.1, 101325.0 - i)
        t = time.ticks_ms()
        with open(TEST, "a") as f:
            f.write(line)
        writes.append(time.ticks_diff(time.ticks_ms(), t))

    avg = sum(writes) / len(writes)
    print("   逐行追加 %d 行：平均 %.1f ms / 最大 %d ms"
          % (rows, avg, max(writes)))
    if avg > 40:
        print("   !! 单次写入偏慢，建议把 central_tx.py 的 SD_PERIOD_MS 调到 500")
    else:
        print("   OK 写入开销可接受，SD_PERIOD_MS = 200 够用")
except Exception as e:
    print("3) 写入失败 ->", e)

# --- 4) 读回校验 ---
try:
    with open(TEST) as f:
        lines = f.read().split("\n")
    body = [l for l in lines if l.strip()]
    print("4) 读回 %d 行（表头1 + 数据%d），末尾一行：" % (len(body), len(body) - 1))
    print("   " + body[-1])
    ok = (len(body) == rows + 1)
    print("   行数校验:", "通过" if ok else "不通过（期望 %d）" % (rows + 1))
except Exception as e:
    print("4) 读回失败 ->", e)

# --- 5) 清理 ---
try:
    os.remove(TEST)
    print("5) 测试文件已删除")
except Exception as e:
    print("5) 删除失败 ->", e)

print("=== SD 自测结束 ===")

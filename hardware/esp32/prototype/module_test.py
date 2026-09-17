import time
import os
from machine import Pin, SPI

# 如果板子里已经有 sdcard.py，这里就能正常 import
# 没有的话我也已经给你放到桌面了，上传到板子即可
try:
    import sdcard
    HAS_SDCARD_LIB = True
except Exception:
    HAS_SDCARD_LIB = False

# ============================================================
# 引脚定义
# ============================================================
# ---- EC11 ----
EC11_S1 = 35
EC11_S2 = 36
EC11_KEY = 41

# ---- TTP223 ----
TTP_PIN = 42

# ---- TF / Micro SD ----
SD_CS   = 45
SD_SCK  = 21
SD_MOSI = 10
SD_MISO = 3

# ============================================================
# EC11 初始化
# ============================================================
ec11_s1 = Pin(EC11_S1, Pin.IN, Pin.PULL_UP)
ec11_s2 = Pin(EC11_S2, Pin.IN, Pin.PULL_UP)
ec11_key = Pin(EC11_KEY, Pin.IN, Pin.PULL_UP)

last_s1 = ec11_s1.value()
last_key = ec11_key.value()

# ============================================================
# TTP223 初始化
# ============================================================
ttp = Pin(TTP_PIN, Pin.IN)
last_ttp = ttp.value()

# ============================================================
# SD 初始化（只做在线 / 挂载检测）
# ============================================================
sd_ok = False
sd_msg = "未测试"

if HAS_SDCARD_LIB:
    try:
        spi_sd = SPI(
            2,
            baudrate=1000000,
            polarity=0,
            phase=0,
            sck=Pin(SD_SCK),
            mosi=Pin(SD_MOSI),
            miso=Pin(SD_MISO)
        )
        cs = Pin(SD_CS, Pin.OUT)
        cs.value(1)

        sd = sdcard.SDCard(spi_sd, cs)
        os.mount(sd, "/sd")
        sd_ok = True
        sd_msg = "OK"
        print("✅ TF/SD 已挂载")
        try:
            print("SD list:", os.listdir("/sd"))
        except Exception as e:
            print("列目录失败:", e)
    except Exception as e:
        sd_msg = "未在线或未插卡: {}".format(e)
        print("❌ TF/SD 未在线或未插卡:", e)
else:
    sd_msg = "没有 sdcard.py"
    print("⚠️ 没有 sdcard.py，无法测试 TF/SD")

print("================================================")
print("三合一模块测试开始")
print("EC11: 旋转看 CW/CCW, 按下看 KEY")
print("TTP223: 触摸看 TOUCH ON/OFF")
print("SD状态:", sd_msg)
print("================================================")

# ============================================================
# 主循环
# ============================================================
while True:
    # ---------------- EC11 旋转检测 ----------------
    s1 = ec11_s1.value()
    s2 = ec11_s2.value()

    if s1 != last_s1:
        time.sleep_ms(2)  # 简单防抖
        s1n = ec11_s1.value()
        s2n = ec11_s2.value()

        if s1n != last_s1:
            if s1n == 0:
                if s2n == 1:
                    print("EC11 -> CW")
                else:
                    print("EC11 -> CCW")
            last_s1 = s1n

    # ---------------- EC11 按键检测 ----------------
    k = ec11_key.value()
    if k != last_key:
        time.sleep_ms(10)
        kn = ec11_key.value()
        if kn != last_key:
            if kn == 0:
                print("EC11 -> KEY")
            last_key = kn

    # ---------------- TTP223 检测 ----------------
    t = ttp.value()
    if t != last_ttp:
        time.sleep_ms(10)
        tn = ttp.value()
        if tn != last_ttp:
            if tn == 1:
                print("TTP223 -> TOUCH ON")
            else:
                print("TTP223 -> TOUCH OFF")
            last_ttp = tn

    time.sleep_ms(5)

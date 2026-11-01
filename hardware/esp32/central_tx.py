"""ESP32 integrated flight telemetry collector."""
import machine
import time
import math
import os
import json
import sdcard
from mpu6050 import MPU6050
from drv import BME280
import buzzer_drv
# ---- 显示屏：已从 ST7735 (SPI) 换成 SSD1306 (I2C) ----
# 换屏原因：OLED 只要 2 根线，而且 TFT 原来占的 6 个脚能腾出 4 个给别的外设。
# ★ 引脚冲突提醒：OLED 用的是 GPIO 11/12（原 TFT 的 MOSI/SCK），
#   **绝不能**照搬另一个项目的 16/17 —— 这里 GPIO17 是 LoRa 的 TX，会把无线顶掉。
try:
    import ssd1306
except Exception:
    ssd1306 = None
try:
    import neopixel
except Exception:
    neopixel = None

# Confirm these pins against the actual board wiring before deployment.
MPU_SCL, MPU_SDA = 9, 3
BME_SCL, BME_SDA = 1, 2
# GP10 UART: module TXD -> ESP32 GPIO38, module RXD <- ESP32 GPIO39
GPS_TX, GPS_RX = 39, 38
LORA_TX, LORA_RX = 17, 18
# TF 卡实际接线（实测确认）：CS=14, SCK=21, MOSI=47, MISO=4
# 踩坑记录：
#   1) MOSI/MISO 一开始被接反（按 48/47 接时 CMD0 无响应，换成 47/48 立刻返回 0x01）
#   2) MISO 一度接在 GPIO48 —— 那是开发板板载 WS2812 RGB 灯的数据脚。
#      WS2812 会把这根线上的 SPI 数据当成颜色吃掉（灯乱亮），
#      同时它的输入级给 MISO 增加了容性负载与漏电，导致 SD 初始化时好时坏
#      （实测同一配置出现过 10/10 全失败、也出现过 8/8 全成功）。
#      现已把 MISO 挪到空闲的 GPIO4，GPIO48 腾出来专门驱动 RGB 指示灯。
SD_CS, SD_SCK, SD_MOSI, SD_MISO = 14, 21, 47, 4

# ---- OLED SSD1306 128x64（I2C）----
# 引脚取自原 TFT 腾出来的位置：SCL=12、SDA=11（原本是 TFT 的 SCK / MOSI）。
# 这两个脚已经在显示接口上、且实测安全（不是 flash/PSRAM/strapping 脚），
# 接线不用绕到板子另一头，也不碰 LoRa 的 17/18。
OLED_SCL, OLED_SDA = 12, 11
OLED_ADDR = 0x3C
OLED_W, OLED_H = 128, 64

mpu = bme = gps = lora = sd = None
sd_ok = gps_fix = lora_ok = False
gps_time = "NO_TIME"
gps_line = ""
lat = lon = "--"
speed = 0.0
satellites = 0
fused_alt = 0.0
baro_alt = 0.0
baro_ref = None
baro_fix_ref = None
gps_alt = 0.0
oled = None
last_tft_ms = 0
last_gps_beep_ms = 0
last_gps_fix_ms = 0
last_lora_beep_ms = 0
last_quick_ms = 0
last_full_ms = 0
last_sd_ms = 0
tx_count = 0
last_stat_ms = 0
listen_count = 0
cmd_count = 0
# ★ 噪声统计（诊断 A28 用）：[0]=未识别行数，[1]=样本。
#   为什么需要它：GPIO18 悬空/模块没驱动 TXD 时会收到随机字节，
#   旧代码对**任何**行都 cmd_count += 1，于是噪声被当成"思澈下发的命令"，
#   让人误判链路是通的（实测 80 秒涨 36 个假命令，却一条 CMD PING 都没打印）。
junk_stat = [0, []]
# ★ 接收行缓冲。UART 一次 read 可能只拿到半条命令，必须攒到 \n 才算一条。
#   用可变的 list 装，省掉 global 声明。
rx_line_buf = [""]
yaw = 0.0
last_imu_ms = 0
lat_f = 0.0    # 数值经纬度（度），供思澈雷达计算
lon_f = 0.0
last_b_cmd = ""
last_b_cmd_ms = 0
gps_fix_played = False
lora_ok_played = False

# ---- LoRa 链路健康判定（双向探测）----
# 关键教训：UART.write() 只是把数据交给本机发送 FIFO/移位寄存器，
# 模块没插、模块断电、超出距离、思澈关机，write 全都不会报错。
# 所以"发送没抛异常"绝对不能当链路判据——之前正是这样写的，
# 结果模块拔掉后 ESP32 屏幕上仍然一直显示 L:OK。
# ---- LoRa 链路存活检测开关（默认关闭）----
# 这套"箭载端确认地面端是否在线"的检测默认关闭，因为实测做不稳：
#   · UART.write() 永远不会报错——模块没插、模块断电、超距、对端关机都不抛异常，
#     所以"发送没报错"不能当链路判据，必须靠对端回包；
#   · 这条链路上对端回包很难稳定收到：短探测包会被 LoRa 透明模块攒住再发
#     （3 字节到达率仅 25%，加长到 12 字节才 53%）；主动探测又会因半双工把模块
#     从"接收"切回"发送"、打断自己正要收的回包（一个窗口内发 3 次反而全部丢失）；
#   · 遥测把发射时隙占满时，箭载端平均 10 秒以上才能收到一条下行包，判定迟滞很大，
#     实测还会误报"断开"。
# 因此改为开关、默认关闭。关闭时：不向地面端上报链路字段、不播放链路告警音，
# 遥测照常发送（本项对传输速度零影响）。需要排查时把开关置 True 即可。
LINK_MONITOR = False
LINK_TIMEOUT_MS = 60000    # 开关打开时：60 秒内收到过地面端回包即认为对端在线
# 遥测 L 字段专用的新鲜度窗口（5 分钟），比 LINK_TIMEOUT_MS 长得多。
# 为什么不能用 60 秒：思澈的兜底 PING 是"每 6000 个主循环 tick"发一次，
# 而主循环会被 LCD 刷屏拖慢，实测真实间隔可超过 2 分钟 —— 用 60 秒会误报断链。
LINK_REPORT_MS = 300000

# ---- 双工模式开关（★ 必须与 A28 的 MD0/MD1 接线同步）----
#   False = 半双工（MD0=0, MD1=0）：模块同一时间只能单向传输，
#           所以必须周期性留出"纯接收窗口"收思澈下发的命令，
#           代价是这期间遥测停发（实测吞吐只有设计值的一半左右）。
#   True  = 全双工（MD0=0,MD1=1 或 MD0=1,MD1=0）：手册 7.2.3 明确
#           "模块可以双向传输，数据可以同时收发"——收发互不干扰，
#           **接收窗口可以整个删掉**，遥测连续发送、命令随时下发。
# ★★ 切到 True 之前，硬件必须已经是全双工接线，否则两端同时发射互相撞掉。
FULL_DUPLEX = True

# 全双工自动兜底判定窗口（毫秒）。
# 全双工下如果**从开机到现在一条命令都没收到**，说明硬件其实还是半双工——
# 模块收发不能同时进行，持续发射就等于彻底听不见地面端。
# 这时自动退回半双工窗口模式。窗口模式一定能收，所以只会单向收敛，不会震荡。
# 90 秒晚于"思澈开机必发的 S:PING"，正常情况下不会误判。
# ★ 有这层兜底，即使开关和接线一时对不上，也只是掉速，绝不会收不到点火命令。
FD_FALLBACK_MS = 90000

# 遥测发送周期。
#   快包 100ms（10 Hz）—— 姿态显示更跟手。28 字节能整包进 128 字节 FIFO，
#                          **不阻塞**，所以这一档可以放心拉快。
#   完整包 2000ms —— ★ 曾试过 1000ms，结果屏上出现"一秒流畅、然后顿一下"。
#                     原因：完整包 216 字节 > FIFO，在 9600 波特下要占掉
#                     约 225ms 串口时间，是**唯一会阻塞**的一包；周期减半
#                     就等于顿挫频率翻倍。改回 2000ms 后周期感恢复。
#   根治办法：串口 9600 → 115200（模块 SPEED 寄存器 + 思澈固件
#             cfsetispeed 两边一起改），216 字节只要 19ms，顿挫基本消失。
# 带宽核算：10×28B + 0.5×216B ≈ 388 B/s；9600 8N1 = 960 B/s，占用约 40%。
QUICK_MS = 100 if FULL_DUPLEX else 250
FULL_MS = 2000
# 全双工下 TF 卡的批量补写周期（半双工靠接收窗口的下降沿触发，全双工没有窗口了）
SD_FLUSH_MS = 3000
last_peer_ms = 0           # 最近一次收到地面端有效回包的时刻
peer_count = 0             # 收到的地面端回包总数
tx_fail = False            # 上一次遥测写入是否抛异常
link_ever_ok = False       # 本次上电后是否曾经确认过链路

# ---- 板载 RGB 指示灯（WS2812 @ GPIO48）----
# 每个任务阶段一个颜色，现场扫一眼就知道现在到哪一步了：
#
#   阶段        B:* 命令   颜色            含义
#   ----        --------   --------------  --------------------------------
#   预备        READY      绿 / 红         ★ 这两个是**卫星连接状态**：
#                                          绿 = GPS 已定位，红 = 还没定位
#   准备        ARM        橙黄 (255,160,0)  已解锁、等待点火
#   点火        IGNITE     白   (255,255,255) 全亮
#   灭火        ABORT      蓝   (0,80,255)
#   结束        END        紫   (150,0,255)
#
# ★★ 优先级规则（主人定的）★★
#   卫星连接状态**只在"预备"阶段显示**。
#   一旦进入"准备"（预备点火）之后，就只显示该步骤自己的颜色，
#   不再切回连接状态 —— 否则飞行中灯还在绿/红闪，看不出到哪一步了。
#   任务结束复位回到"预备"后，才重新开始显示连接状态。
RGB_PIN = 48

PHASE_STANDBY = "READY"     # 预备（待命）
PHASE_ARM = "ARM"           # 准备
PHASE_IGNITE = "IGNITE"     # 点火
PHASE_ABORT = "ABORT"       # 灭火
PHASE_END = "END"           # 结束

RGB_COLOR_STANDBY_FIX = (0, 200, 0)       # 预备 + 已定位 → 绿
RGB_COLOR_STANDBY_NOFIX = (200, 0, 0)     # 预备 + 未定位 → 红
RGB_COLOR_ARM = (255, 160, 0)             # 准备 → 橙黄
RGB_COLOR_IGNITE = (255, 255, 255)        # 点火 → 白（全亮）
RGB_COLOR_ABORT = (0, 80, 255)            # 灭火 → 蓝
RGB_COLOR_END = (150, 0, 255)             # 结束 → 紫
RGB_OFF_COLOR = (0, 0, 0)

rgb_led = None
rgb_now = (0, 0, 0)          # 当前实际点亮的颜色
mission_phase = PHASE_STANDBY  # 当前任务阶段（由思澈下发的 B:* 更新）


def rgb_set_color(color):
    """把 RGB 设成指定颜色。写入失败不影响主逻辑，也不会重复写同一个颜色。"""
    global rgb_now
    if rgb_led is None:
        return
    if color == rgb_now:
        return
    try:
        rgb_led[0] = color
        rgb_led.write()
        rgb_now = color
    except Exception as e:
        print("RGB ERR", e)


def rgb_set(on):
    """兼容旧调用：亮 = 白，灭 = 黑。"""
    rgb_set_color(RGB_COLOR_IGNITE if on else RGB_OFF_COLOR)


def rgb_apply_phase():
    """按当前阶段决定 RGB 颜色（含"连接状态只在预备阶段显示"的规则）。

    主循环里每轮都调：
      · 预备阶段 → 颜色跟着 gps_fix 变（绿/红），这就是"卫星连接状态"
      · 其它阶段 → 该阶段的固定颜色，不会再被 gps_fix 影响
    """
    if mission_phase == PHASE_STANDBY:
        rgb_set_color(RGB_COLOR_STANDBY_FIX if gps_fix else RGB_COLOR_STANDBY_NOFIX)
    elif mission_phase == PHASE_ARM:
        rgb_set_color(RGB_COLOR_ARM)
    elif mission_phase == PHASE_IGNITE:
        rgb_set_color(RGB_COLOR_IGNITE)
    elif mission_phase == PHASE_ABORT:
        rgb_set_color(RGB_COLOR_ABORT)
    elif mission_phase == PHASE_END:
        rgb_set_color(RGB_COLOR_END)


def sd_try_mount():
    """尝试识别并挂载 TF 卡。返回 True 表示已就绪。

    抽出来单独一个函数，是为了支持"热插拔自动重试"：
    开机时若初始化失败，主循环会每 SD_RETRY_MS 自动再试一次，
    这样现场把卡拔出来擦一下、重新插紧后**不用重启板子**就能自动认到。

    卡其实常常是好的——实测同一张卡在干净环境下 5/5 次成功、只要 67ms，
    但开机时它排在 TFT/LoRa/GPS 之后初始化，SoftSPI 时序容易被干扰而首次失败。
    """
    global sd, sd_ok
    try:
        spi = machine.SoftSPI(baudrate=400000, polarity=0, phase=0,
                              sck=machine.Pin(SD_SCK), mosi=machine.Pin(SD_MOSI),
                              miso=machine.Pin(SD_MISO, machine.Pin.IN,
                                               machine.Pin.PULL_UP))
        sd = sdcard.SDCard(spi, machine.Pin(SD_CS, machine.Pin.OUT), 400000)
        try:
            os.mount(sd, "/sd")
        except Exception:
            # 已经挂过就卸载重挂，避免 "already mounted"
            try:
                os.umount("/sd")
            except Exception:
                pass
            os.mount(sd, "/sd")
        sd_ok = True
        return True
    except Exception as e:
        sd_ok = False
        sd = None
        for _p in (SD_CS, SD_SCK, SD_MOSI, SD_MISO):
            try:
                machine.Pin(_p, machine.Pin.IN)
            except Exception:
                pass
        return False


def init_hw():
    global mpu, bme, gps, lora, sd, sd_ok, oled, rgb_led
    # ---- OLED 初始化（I2C，SSD1306 128x64 @0x3C）----
    # 用**硬件 I2C**：128x64 整帧只有 1KB，I2C 400kHz 下几十毫秒就能刷完。
    # 千万别用 SoftI2C —— 软件位翻转会慢一个数量级，拖慢主循环。
    if ssd1306:
        try:
            oled_i2c = machine.I2C(0, scl=machine.Pin(OLED_SCL),
                                   sda=machine.Pin(OLED_SDA), freq=400000)
            found = oled_i2c.scan()
            oled = ssd1306.SSD1306_I2C(OLED_W, OLED_H, oled_i2c, OLED_ADDR)
            oled.fill(0)
            oled.text("FLIGHT COMPUTER", 8, 26, 1)
            oled.text("OLED SCL=%d SDA=%d" % (OLED_SCL, OLED_SDA), 0, 40, 1)
            oled.show()
            print("OLED OK (i2c scan=%s)" % [hex(a) for a in found])
            if OLED_ADDR not in found:
                print("OLED WARN: 0x%02X 不在扫描结果里，检查 SCL/SDA 接线" % OLED_ADDR)
        except Exception as e:
            print("OLED ERR", e)
    else:
        print("OLED: ssd1306.py 没找到")
    mpu_bus = machine.SoftI2C(scl=machine.Pin(MPU_SCL), sda=machine.Pin(MPU_SDA), freq=100000)
    bme_bus = machine.SoftI2C(scl=machine.Pin(BME_SCL), sda=machine.Pin(BME_SDA), freq=100000)
    try:
        mpu = MPU6050(mpu_bus); print("MPU6050 OK")
    except Exception as e: print("MPU6050 ERR", e)
    try:
        bme = BME280(bme_bus)
        print("BME280/BMP280", hex(bme.chip_id) if bme.present else "N/A")
    except Exception as e: print("BME ERR", e)
    try:
        gps = machine.UART(2, baudrate=9600, tx=GPS_TX, rx=GPS_RX, timeout=0, rxbuf=1024)
        print("GPS UART OK")
    except Exception as e: print("GPS ERR", e)
    try:
        lora = machine.UART(1, baudrate=9600, tx=LORA_TX, rx=LORA_RX)
        print("LoRa UART OK")
    except Exception as e: print("LoRa ERR", e)
    # ---- TF 卡初始化（带重试，但有总时间上限）----
    # 这张卡偶发进不了就绪态，重试能显著提高成功率；但重试本身也要花时间，
    # 必须给总时限——否则卡没插好时会把开机拖住几十秒（实测出现过 30 秒以上，
    # 表现为"ESP32 卡住了、屏幕不动"）。
    # 配合 sdcard.py 里把单次 v1/v2 超时从 5 秒压到 0.6 秒，这里再限总时长 3 秒。
    # 开机没认到也没关系：主循环会每 SD_RETRY_MS 自动重试（支持热插拔）。
    sd_deadline = time.ticks_add(time.ticks_ms(), 3000)
    for sd_try in range(4):
        if time.ticks_diff(sd_deadline, time.ticks_ms()) <= 0:
            break
        if sd_try_mount():
            print("SD OK (try %d)" % (sd_try + 1))
            break
        print("SD try %d/4 failed" % (sd_try + 1))
        time.sleep_ms(120)
    if not sd_ok:
        print("SD N/A now - will keep retrying in background")
    # 板载 RGB：上电即"预备"阶段 → 显示卫星连接状态（未定位=红，定位=绿）。
    # WS2812 掉电后状态随机且会锁存，不做这一步开机可能一直乱亮。
    if neopixel is not None:
        try:
            rgb_led = neopixel.NeoPixel(machine.Pin(RGB_PIN), 1)
            rgb_apply_phase()
            print("RGB LED OK (phase=%s)" % mission_phase)
        except Exception as e:
            rgb_led = None
            print("RGB ERR", e)


def parse_gps():
    global gps_line, gps_fix, gps_time, satellites, fused_alt, lat, lon, speed, last_gps_fix_ms, gps_alt
    global lat_f, lon_f
    if gps is None:
        return
    n = gps.any()
    if n:
        try:
            raw = gps.read(n)
            if isinstance(raw, tuple):
                raw = "".join(str(x) for x in raw)
            elif isinstance(raw, bytes):
                raw = raw.decode()
            elif not isinstance(raw, str):
                raw = str(raw)
            gps_line += raw
        except Exception:
            gps_line = ""
    while "\n" in gps_line:
        line, gps_line = gps_line.split("\n", 1)
        p = line.strip().split(",")
        if (line.startswith("$GNGGA") or line.startswith("$GPGGA")) and len(p) > 9:
            gps_fix = p[6] in ("1", "2")
            satellites = int(p[7]) if p[7].isdigit() else 0
            if gps_fix:
                try: gps_alt = float(p[9])
                except Exception: pass
                # GGA 本身也带时间与经纬度：不依赖 RMC 语句也能拿到坐标
                try:
                    hh = (int(p[1][0:2]) + 8) % 24
                    gps_time = "%02d:%02d:%02d" % (hh, int(p[1][2:4]), int(p[1][4:6]))
                except Exception: pass
                try:
                    la = int(p[2][:2]) + float(p[2][2:]) / 60.0
                    lo = int(p[4][:3]) + float(p[4][3:]) / 60.0
                    if p[3] == "S": la = -la
                    if p[5] == "W": lo = -lo
                    lat_f = la
                    lon_f = lo
                    last_gps_fix_ms = time.ticks_ms()
                except Exception: pass
        elif (line.startswith("$GNRMC") or line.startswith("$GPRMC")) and len(p) > 9:
            if p[2] == "A":
                gps_fix = True
                last_gps_fix_ms = time.ticks_ms()
                try:
                    hh = (int(p[1][0:2]) + 8) % 24
                    gps_time = "%02d:%02d:%02d" % (hh, int(p[1][2:4]), int(p[1][4:6]))
                except Exception: pass
                try:
                    la = int(p[3][:2]) + float(p[3][2:]) / 60.0
                    lo = int(p[5][:3]) + float(p[5][3:]) / 60.0
                    if p[4] == "S": la = -la
                    if p[6] == "W": lo = -lo
                    lat_f = la
                    lon_f = lo
                    lat = ("N" if p[4] == "N" else "S") + "%.5f" % abs(la)
                    lon = ("E" if p[6] == "E" else "W") + "%.5f" % abs(lo)
                except Exception: lat = lon = "--"
                try: speed = float(p[7]) * 0.514444
                except Exception: speed = 0.0
            else:
                gps_fix = False


def read_data():
    global baro_alt, baro_ref, fused_alt, baro_fix_ref, gps_alt
    global yaw, last_imu_ms
    gps_base_alt = 0.0
    a = mpu.read_accel_data() if mpu else {"x": 0, "y": 0, "z": 1}
    g = mpu.read_gyro_data() if mpu else {"x": 0, "y": 0, "z": 0}
    pitch = math.degrees(math.atan2(a["y"], a["z"]))
    roll = math.degrees(math.atan2(-a["x"], math.sqrt(a["y"] ** 2 + a["z"] ** 2)))

    # 偏航角：MPU6050 无磁力计，用陀螺仪 Z 轴角速度积分得到相对偏航。
    now_ms = time.ticks_ms()
    dt = time.ticks_diff(now_ms, last_imu_ms) / 1000.0
    if last_imu_ms == 0 or dt <= 0 or dt > 0.5:
        dt = 0.01
    last_imu_ms = now_ms
    gz = g["z"] if isinstance(g, dict) else 0.0
    yaw += gz * dt
    if abs(gz) < 1.5:
        yaw *= 0.999   # 静止时缓慢回零，抑制陀螺漂移
    while yaw > 180.0: yaw -= 360.0
    while yaw < -180.0: yaw += 360.0
    temp = pressure = None
    if bme and bme.present:
        try:
            temp, pressure, _ = bme.read()
            if pressure and baro_ref is None: baro_ref = pressure
            if pressure and baro_ref:
                baro_alt = 44330.0 * (1.0 - (pressure / baro_ref) ** (1.0 / 5.255))
        except Exception: pass
    if gps_fix:
        # GPS 海拔作为基准，校准气压绝对高度偏移；之后以气压相对变化为主
        if baro_fix_ref is None and baro_alt is not None:
            baro_fix_ref = baro_alt
            gps_base_alt = gps_alt
        if baro_fix_ref is not None:
            # 气压相对高度 + GPS 绝对海拔基准，得到平滑融合高度
            fused_alt = (baro_alt - baro_fix_ref) + gps_alt
        else:
            fused_alt = baro_alt
    else:
        baro_fix_ref = None
        fused_alt = baro_alt
    return pitch, roll, temp, pressure, a, g


sd_record = False
sd_file_id = 0
sd_path = None
SD_PERIOD_MS = 200        # SD 采样周期（见主循环里关于写卡开销的说明）
SD_PERIOD_MAX_MS = 8000   # 自适应上限：卡极慢时最多降到 0.125 Hz（保链路）
SD_FLUSH_ROWS = 10        # 攒够 10 行（约 2 秒）才落盘一次
SD_FLUSH_AGE_MS = 5000    # 缓冲最多滞留 5 秒就落盘。
                          #   原来设 1 秒，实测卡变慢后每次只攒到 1 行就写，
                          #   完全失去攒批意义（1 行也要 1.2 秒）。
SD_FORCE_ROWS = 60        # 缓冲涨到这个量就强写，防止一直卡在接收窗口里不出窗
SD_SLOW_MS = 600          # 单次落盘超过这个时间即认为卡太慢，自动降采样率
in_listen_now = False     # 主循环每轮更新：当前是否处于 LoRa 接收窗口
sd_period_ms = SD_PERIOD_MS   # 实际采样周期（会被自适应下调）
sd_retry_ms = 0           # 上次尝试重挂卡的时刻（热插拔重试用）
SD_RETRY_MS = 20000       # 卡没认出来时，每 20 秒自动重试一次
sd_err = 0                # 连续写卡失败次数，超限就停止记录保护主循环
sd_buf = []               # 待落盘的行缓冲
sd_buf_ms = 0             # 缓冲第一行的时刻（用于"滞留超时也落盘"）
SD_HEADER = ("time,gps_fix,lora_ok,lat,lon,alt_m,speed_ms,temp_c,pressure_pa,"
             "pitch,roll,yaw,ax,ay,az,gx,gy,gz\n")


def sd_flush():
    """把缓冲的行一次写入。单行 open→write→close 实测约 120ms（FAT 开销大），
    200ms 周期下写一行就要占掉 60% 的主循环时间，会拖垮 LoRa 时序与刷屏。
    攒 10 行写一次，摊薄到约 15%，代价是掉电最多丢 2 秒数据。

    ⚠ 接收窗口内一律不写卡：
    写卡一次阻塞 200~500ms，而 SoftSPI 是纯 CPU 位翻转。9600 波特下 500ms
    就是 480 字节，远超 UART 的 128 字节 FIFO 与 MicroPython 的 256 字节 rxbuf
    ——窗口期间进来的按键命令会被直接丢掉。实测窗口内写卡时，80 秒里
    一条上行都收不到。所以这里发现正处于接收窗口就先返回，等窗口过去再写，
    缓冲继续攒着（不丢数据、也不影响遥测发射节奏）。"""
    global sd_path, sd_err, sd_record, sd_ok, sd_buf, sd_period_ms
    if not sd_buf:
        return
    if in_listen_now and len(sd_buf) < SD_FORCE_ROWS:
        return          # 让位给接收窗口，出窗后立刻补写
    try:
        if sd_path is None:
            sd_path = "/sd/telemetry_" + str(sd_file_id) + ".csv"
            with open(sd_path, "w") as f:
                f.write(SD_HEADER)
            print("SD file ->", sd_path)
        t0 = time.ticks_ms()
        with open(sd_path, "a") as f:
            f.write("\n".join(sd_buf) + "\n")
        dt = time.ticks_diff(time.ticks_ms(), t0)
        if dt > SD_SLOW_MS:
            # 卡太慢：写一次就占掉几百毫秒甚至 1 秒，会把主循环（以及 LoRa 收发）
            # 拖垮。这里自动降低采样率，用数据密度换回时序，
            # 保证"存数据"和"准确收令"两件事都不至于互相拖死。
            if sd_period_ms < SD_PERIOD_MAX_MS:
                sd_period_ms = min(sd_period_ms * 2, SD_PERIOD_MAX_MS)
                print("SD slow %dms -> period %dms" % (dt, sd_period_ms))
        elif dt < 150 and sd_period_ms > SD_PERIOD_MS:
            sd_period_ms //= 2          # 卡恢复了就慢慢调回来
        if dt > 300:
            print("SD flush %d rows %dms (period=%d)" % (len(sd_buf), dt, sd_period_ms))
        sd_buf = []
        sd_err = 0
    except Exception as e:
        sd_buf = []
        sd_err += 1
        if sd_err <= 3:
            print("SD write ERR", e)
        if sd_err >= 5:
            # 卡被拔掉/写坏时，不能每 200ms 报一次错把主循环和串口刷爆
            sd_record = False
            sd_ok = False
            print("SD disabled after 5 consecutive errors")


def log_row(row):
    global sd_buf, sd_buf_ms
    if not sd_ok or not sd_record: return
    if not sd_buf:
        sd_buf_ms = time.ticks_ms()
    sd_buf.append(",".join(str(x) for x in row))
    # 攒够 5 行、或缓冲已滞留满 1 秒，就落盘
    if len(sd_buf) >= SD_FLUSH_ROWS or \
            time.ticks_diff(time.ticks_ms(), sd_buf_ms) >= SD_FLUSH_AGE_MS:
        sd_flush()


def send_ack(tag):
    """回执：思澈据此确认 ESP32 已收到按键命令（并已在 LCD 上显示）。"""
    if lora is None: return
    try:
        lora.write(("A:" + tag + "\n").encode())
    except Exception:
        pass


def handle_sd_cmd(line):
    """处理思澈下发的命令：S:NEW<id> 新建文件记录，S:STOP 停止；B:* 播放音效。"""
    global sd_record, sd_file_id, sd_path
    global last_b_cmd, last_b_cmd_ms, mission_phase
    if line.startswith("B:"):
        body = line[2:].strip()
        now = time.ticks_ms()

        # ★★ 去重判据：**按"阶段是否变化"**，不再按时间 ★★
        #
        #  原来判的是"同一条命令 2.5 秒内不重放"。那个判据有个致命问题：
        #  思澈为了命中 ESP32 的接收窗口会把同一条命令重发十几次，
        #  一旦跨越 2.5 秒，就会被当成"新命令"再响一次 —— 声音乱掉。
        #
        #  改成按阶段判断后：
        #    · 同阶段的重复下发 → 只回执，不重放音效（思澈可以放心持续重发）
        #    · 阶段真的变了     → 一定响，且保证每个阶段都响得到
        #
        #  ★ 这正是"每步音效缺失"的正解：思澈那边现在可以把每个阶段
        #    排队发满，ESP32 这边按阶段去重，两边配合就不会漏也不会重。
        last_b_cmd = body
        last_b_cmd_ms = now
        if body == mission_phase:
            send_ack(body)          # 阶段没变：只回执，不重放
            return True

        mission_phase = body
        print("CMD RX B:" + body + " (phase changed)")
        # ★★ 一律走非阻塞 play()，一声都不 sleep ★★
        # 原来这里是 for + buzzer_drv.beep()，而 beep() 内部是 time.sleep_ms()，
        # 一段旋律会把主循环整段卡住 700ms 以上：期间不收 LoRa 命令、不发遥测，
        # 结果就是"蜂鸣器丢步"（边沿触发的阶段音效错过就永久没了，不会补）。
        # 改成 play() 后音效只是排队，主循环照常跑；队列保证每个阶段都响得到。
        if body == "READY":
            # 回到待命/预备：上扬启动音，和确认/点火/灭火/结束区分
            buzzer_drv.play(((660, 120, 55), (880, 120, 55),
                             (1175, 120, 55), (1568, 120, 55)))
        elif body == "ARM":
            buzzer_drv.play(((440, 600, 120),))        # 准备：低音长鸣（点火确认）
        elif body == "IGNITE":
            buzzer_drv.play(((3200, 90, 55),) * 5)     # 点火：5 声尖锐
        elif body == "END":
            buzzer_drv.play(((1200, 150, 55), (1000, 150, 55),
                             (800, 150, 55), (600, 150, 55)))   # 结束：4 声递降
        elif body == "ABORT":
            buzzer_drv.play(((300, 300, 100),) * 2)    # 灭火：2 声低沉
        rgb_apply_phase()           # RGB 跟着阶段走（预备阶段显示连接状态）
        send_ack(body)
        print("CMD", body)
        return True
    if not line.startswith("S:"):
        return False
    body = line[2:].strip()
    if body == "PING":
        send_ack("PONG")          # 链路自检：证明思澈→ESP32 双向可达
        print("CMD PING")
    elif body == "STOP":
        sd_flush()                # 先落盘缓冲，再停记录
        sd_record = False
        print("SD STOP")
    elif body.startswith("NEW"):
        sd_flush()                # 旧文件的缓冲先落盘，再切新文件
        try: sd_file_id = int(body[3:])
        except Exception: sd_file_id += 1
        sd_path = None
        sd_record = True
        print("SD NEW", sd_file_id)
        send_ack("SD" + str(sd_file_id))
    return True


def poll_lora_cmd():
    """读取思澈经 LoRa 下发的按键命令（低延迟优先处理）。"""
    global cmd_count, last_peer_ms, peer_count, link_ever_ok
    if lora is None: return
    got = False
    try:
        # 一次排空接收缓冲，避免命令排队
        while True:
            n = lora.any()
            if not n: break
            raw = lora.read(n)
            if isinstance(raw, tuple): raw = "".join(str(x) for x in raw)
            elif isinstance(raw, bytes): raw = raw.decode()
            elif not isinstance(raw, str): raw = str(raw)
            # ★ 必须做行缓冲：一次 read 可能只拿到半条命令，
            #   按 read 边界切会把它劈成两段"噪声"（实测 90 秒凭空涨 10 条 junk）。
            #   攒够完整的一行（见到 \n）才处理。
            rx_line_buf[0] += raw.replace("\r", "")
            if len(rx_line_buf[0]) > 512:
                rx_line_buf[0] = ""      # 异常长行：丢弃，防止缓冲无限增长
            while "\n" in rx_line_buf[0]:
                line, rx_line_buf[0] = rx_line_buf[0].split("\n", 1)
                line = line.strip()
                if not line: continue
                # 思澈回包：P:1 = 探测应答，A:xx = 命令回执。
                # 这是"链路双向可达"唯一可靠的证据——本机发送成功不算数。
                # 回包不是命令，不走 handle_sd_cmd，也不计进 cmd_count。
                if len(line) >= 2 and line[0] in ("P", "A") and line[1] == ":":
                    last_peer_ms = time.ticks_ms()
                    peer_count += 1
                    link_ever_ok = True
                    got = True
                    continue
                # ★ 只认 B:/S: 两种真实命令前缀（handle_sd_cmd 也只处理这两种）。
                #   其余一律算噪声，**不计入 cmd_count**——否则链路状态会说谎。
                if len(line) >= 2 and line[0] in ("B", "S") and line[1] == ":":
                    handle_sd_cmd(line)
                    got = True
                    cmd_count += 1
                    # ★★ 收到真实命令 = 地面端确实在线，直接当作链路存活证据。
                    #    这比主动探测可靠得多：不用额外发探测包去抢半双工信道
                    #    （老注释里"探测打断收包"的坑就是这么来的）。
                    #    即使 LINK_MONITOR 关着也生效，让 lora_ok 与遥测 L 字段说真话。
                    last_peer_ms = time.ticks_ms()
                    link_ever_ok = True
                else:
                    junk_stat[0] += 1
                    if len(junk_stat[1]) < 6:
                        junk_stat[1].append(line[:24])
    except Exception:
        pass
    return got



def play_melody(notes):
    for freq, duration, pause in notes:
        buzzer_drv.beep(freq, duration, pause)


def update_tft(pitch, roll, temp, pressure):
    """刷屏（函数名保留 update_tft 不动，减少改动面；实际画的是 OLED）。

    SSD1306 128x64 单色：**16 字符 × 8 行**，每行 8 像素高。
    行位置 y = 0/8/16/24/32/40/48/56，正好 8 行，一行都不浪费。
    原来是 160x128 彩色 TFT 的 6 行，现在多出 2 行给姿态和电压之外的用途。
    """
    if oled is None: return
    try:
        # 版面：
        #   FLIGHT COMPUTER      ← 标题
        #   G:FIX  SAT16         ← 定位状态 + 卫星数
        #   ALT   123.4m         ← 融合高度
        #   V      12.3m/s       ← 对地速度（原来是和 ALT 挤一行，现在拆开）
        #   BAR 1013.2hPa        ← 气压
        #   TMP    26.6C         ← 温度
        #   P  1.2 R -3.4        ← 姿态 pitch/roll（OLED 多出来的行）
        #   SD:1 REC 12:34:56    ← SD 状态，一眼看出在不在录
        oled.fill(0)
        oled.text("FLIGHT COMPUTER", 0, 0, 1)
        oled.text("G:%-3s SAT%2d" % ("FIX" if gps_fix else "NO", satellites), 0, 8, 1)
        oled.text("ALT %7.1fm" % fused_alt, 0, 16, 1)
        oled.text("V   %7.1fm/s" % speed, 0, 24, 1)
        oled.text("BAR %7.1fhPa" % ((pressure or 0) / 100.0), 0, 32, 1)
        oled.text("TMP %7.1fC" % (temp or 0), 0, 40, 1)
        oled.text("P%5.1f R%5.1f" % (pitch or 0.0, roll or 0.0), 0, 48, 1)
        # SD 状态直接显示在屏幕上，一眼能看出"有没有卡、在不在录"
        if not sd_ok:
            sd_txt = "SD:-- NO CARD"
        elif sd_record:
            sd_txt = "SD:%d REC %s" % (sd_file_id, gps_time[:8] or "--:--:--")
        else:
            sd_txt = "SD:%d stop %s" % (sd_file_id, gps_time[:8] or "--:--:--")
        oled.text(sd_txt[:16], 0, 56, 1)
        oled.show()
    except Exception as e: print("OLED update ERR", e)


def main():
    global lora_ok, last_gps_beep_ms, last_lora_beep_ms
    global gps_fix_played, lora_ok_played, gps_fix, satellites, last_tft_ms
    global last_quick_ms, last_full_ms, last_sd_ms, tx_count, last_stat_ms
    global listen_count, cmd_count
    global last_b_cmd, last_b_cmd_ms
    global peer_count, tx_fail, link_ever_ok
    global sd_record, sd_file_id, sd_path, sd_err
    global in_listen_now, was_in_listen, sd_retry_ms, sd_buf, sd_buf_ms
    init_hw()
    # 开机自检音：确认 ESP32 与蜂鸣器工作
    play_melody(((1200, 120, 80), (1600, 120, 80), (2000, 200, 120)))
    print("BUZZER self-test done")
    # 插卡即自动开始记录。SD 记录是核心功能，不能因为"还没按 KEY1"就一行都不写；
    # 思澈端 KEY1 后续下发 S:NEW<n> 时会切到新文件，用于按任务阶段分段。
    if sd_ok:
        sd_record = True
        sd_path = None
        sd_err = 0
        print("SD recording auto-start -> telemetry_%d.csv" % sd_file_id)
    else:
        print("SD absent: recording off (insert card and reboot)")
    tick = 0
    listen_until = 0
    was_in_listen = False
    last_listen_ms = time.ticks_ms()
    sd_flush_ms = time.ticks_ms()   # 全双工下的 TF 卡批量补写计时
    fd_mode = FULL_DUPLEX           # 运行时双工模式，可被下面的自动兜底降级
    boot_ms = time.ticks_ms()       # "全双工是否真的成立"的判定起点
    while True:
        # 按键命令最高优先级，循环 10ms 一次，最迟 10ms 内响应
        poll_lora_cmd()

        # ★ 蜂鸣器非阻塞推进：每轮就推进一下音符，绝不 sleep。
        #   放在命令之后、其余工作之前，保证音效节奏不受后面那些耗时操作影响。
        buzzer_drv.tick()

        now = time.ticks_ms()

        # ---- 全双工自动兜底 ----
        # 全双工下若从开机到现在**一条命令都没收到**，说明硬件其实还是半双工：
        # 模块收发不能同时进行，持续发射就等于完全听不见地面端。
        # 立刻退回半双工窗口模式——窗口模式一定能收，所以只会单向收敛，不震荡。
        # 有了这层兜底，开关与接线一时对不上也只是掉速，**绝不会收不到点火命令**。
        if fd_mode and not link_ever_ok and \
                time.ticks_diff(now, boot_ms) >= FD_FALLBACK_MS:
            fd_mode = False
            print("★ 全双工不成立（90 秒未收到任何命令），已自动退回半双工窗口模式")
            print("  请检查 MD0/MD1 接线：全双工要 MD0=0,MD1=1 或 MD0=1,MD1=0")

        # ---- 纯接收窗口：只有半双工才需要 ----
        # 命令优先：每 2 秒留 1 秒纯接收窗口。
        # LoRa 空中发射时间远长于 UART 写入时间，200ms 窗口不可靠；
        # 思澈会连续重发约 3.2 秒，确保 B:IGNITE 等命中窗口。
        # 窗口必须够长：模块空中发送时间远大于 UART 写入时间，
        # 400ms 时积压没排完窗口就结束，实测按键命令完全收不到。
        # ★ 全双工（FULL_DUPLEX=True）下收发同时进行、互不干扰，这个窗口
        #   纯属白白丢掉发射时隙，直接跳过。接收不会漏：主循环每轮都调
        #   poll_lora_cmd()，等于 100% 时间在听，比原来 1/3 时间在听更强。
        if fd_mode:
            in_listen = False
        else:
            if listen_until == 0 and time.ticks_diff(now, last_listen_ms) >= 2000:
                listen_until = now + 1000
                last_listen_ms = now
                listen_count += 1
            in_listen = (listen_until != 0 and
                         time.ticks_diff(now, listen_until) < 0)
            if listen_until != 0 and not in_listen:
                listen_until = 0
        in_listen_now = in_listen      # 给 sd_flush() 用：窗口内不写卡

        # 补写时机：
        #   · 半双工：在"接收窗口刚结束"的下降沿补写一次。不能写成每轮都调——
        #     那样缓冲每轮被清空，攒批完全失效（实测退化成每次只写 1 行、
        #     每行还要 1.1 秒）。
        #   · 全双工：**没有窗口了，下降沿永远不会发生**，必须换成定时补写，
        #     否则 TF 卡录制会静默失效——这个坑很隐蔽：卡上不报错，只是没数据。
        #     周期 3 秒，攒批行数与原来同一个量级。
        if fd_mode:
            if sd_buf and sd_record and \
                    time.ticks_diff(now, sd_flush_ms) >= SD_FLUSH_MS:
                sd_flush_ms = now
                sd_flush()
        elif was_in_listen and not in_listen and sd_buf and sd_record:
            sd_flush()
        was_in_listen = in_listen

        # ---- TF 卡热插拔重试 ----
        # 开机没认到卡时不放弃：每 SD_RETRY_MS 自动重挂一次。
        # 这样现场把卡拔出来擦干净金手指、重新插紧后**不用重启板子**就能认到。
        # 只在接收窗口外做，避免干扰 LoRa 收命令。
        if not sd_ok and not in_listen and \
                time.ticks_diff(now, sd_retry_ms) >= SD_RETRY_MS:
            sd_retry_ms = now
            if sd_try_mount():
                print("SD OK (hot-plug retry)")
                sd_record = True
                sd_path = None
                sd_err = 0
                sd_buf = []
                print("SD recording auto-start -> telemetry_%d.csv" % sd_file_id)

        parse_gps()
        if gps_fix and time.ticks_diff(now, last_gps_fix_ms) > 3000:
            gps_fix = False
            satellites = 0
        pitch, roll, temp, pressure, a, g = read_data()

        if lora and not in_listen:
            try:
                # 周期由 QUICK_MS / FULL_MS 决定，随 FULL_DUPLEX 自动切换：
                #   半双工：快包 250ms + 完整包 2000ms（原实测最优档）
                #   全双工：快包 100ms + 完整包 1000ms（吞吐翻倍，姿态 10 Hz）
                # 说明：原来"再快就掉包"（180ms 档反而掉到 1.3 包/秒）其实不是
                # 带宽不够，而是半双工撞包——模块同一时间只能单向传输。
                # 全双工下这个限制消失，所以可以放心提速。
                if time.ticks_diff(now, last_quick_ms) >= QUICK_MS:
                    last_quick_ms = now
                    # 姿态精度提到 0.01°（原来 0.1°）：全双工后带宽富余，
                    # 地面站的姿态曲线更细腻——"更准"直接体现在这里。
                    quick = {"p": round(pitch, 2), "r": round(roll, 2),
                             "y": round(yaw, 2)}
                    lora.write((json.dumps(quick, separators=(",", ":")) + "\n").encode())
                    tx_count += 1
                # 完整包：含加速度/气压/GPS，供思澈做融合计算
                if time.ticks_diff(now, last_full_ms) >= FULL_MS:
                    last_full_ms = now
                    # 经纬度用于思澈端雷达：思澈本地算相对距离/方位，不回传、不额外占带宽
                    # ★ 全双工后带宽富余，整体提高量化精度（"更准"）：
                    #   姿态 0.1°→0.01°，高度 0.1m→0.01m，温度 0.1°C→0.01°C，
                    #   气压 1Pa→0.1Pa，速度 0.1→0.01 m/s。
                    #   其中**气压/高度最值钱**：原来 0.1m 的分辨率在低速段
                    #   会形成明显的量化台阶，看不出真实趋势；现在能看清。
                    full = {"p": round(pitch, 2), "r": round(roll, 2),
                            "y": round(yaw, 2),
                            # 经纬度用 6 位小数（约 0.11m），避免截断掉 GPS 精度
                            "la": round(lat_f, 6), "lo": round(lon_f, 6),
                            "ax": round(a["x"], 3), "ay": round(a["y"], 3), "az": round(a["z"], 3),
                            "a": round(fused_alt, 2), "gps": 1 if gps_fix and satellites >= 4 else 0,
                            "s": satellites,
                            "t": round(temp, 2) if temp is not None else 0,
                            "P": round(pressure / 1000.0, 3) if pressure else 0,
                            "v": round(speed, 2), "T": gps_time}
                    # ★ 链路字段不再受 LINK_MONITOR 开关限制，始终上报。
                    #   为什么现在能放心上报：lora_ok 的判据已改成"收到思澈的真实命令"
                    #   （见 poll_lora_cmd），这是**纯被动观测**——不发探测包、不抢半双工
                    #   信道，比当年那套主动探测可靠得多；而且箭载端根本没有主动探测代码。
                    #   注意：开关 LINK_MONITOR 仍然只留给"链路告警音"，保持关闭，
                    #   避免思澈 PING 间隔偏长时误报断链、乱叫。
                    #   1=最近听到过地面端 / 0=曾经听到过但现在没声了 / -1=从未听到
                    if link_ever_ok:
                        _fresh = (last_peer_ms != 0 and
                                  time.ticks_diff(now, last_peer_ms) < LINK_REPORT_MS)
                        full["L"] = 1 if _fresh else 0
                    else:
                        full["L"] = -1
                    lora.write((json.dumps(full, separators=(",", ":")) + "\n").encode())
                    tx_count += 1
                tx_fail = False
            except Exception:
                tx_fail = True

        # 链路判定：只认"有没有收到思澈的有效回包"（探测应答 / 命令回执），
        # 不看本机 write 是否报错——write 对链路故障完全不敏感。
        if lora is None or tx_fail or last_peer_ms == 0:
            lora_ok = False
        else:
            lora_ok = time.ticks_diff(now, last_peer_ms) < LINK_TIMEOUT_MS

        # SD 记录：默认 200ms 一行。
        # 注意：每写一行都要 open→write→close，SD 走 SoftSPI 时一次 FAT 更新
        # 可能占几十毫秒。若插卡后发现 LoRa 包率明显下降（比如从 4.5 掉到 3 以下），
        # 把 SD_PERIOD_MS 调大到 500，用数据密度换回主循环时序。
        if sd_record and time.ticks_diff(now, last_sd_ms) >= sd_period_ms:
            last_sd_ms = now
            log_row((gps_time, 1 if gps_fix else 0, int(lora_ok), lat, lon, "%.1f" % fused_alt,
                     "%.1f" % speed, "%.1f" % temp if temp is not None else "",
                     "%.1f" % pressure if pressure is not None else "",
                     "%.2f" % pitch, "%.2f" % roll, 0,
                     a["x"], a["y"], a["z"], g["x"], g["y"], g["z"]))

        if time.ticks_diff(now, last_tft_ms) >= 300:
            last_tft_ms = now
            update_tft(pitch, roll, temp, pressure)

        # 每 10 秒报告发送计数，便于对比思澈接收数评估丢包
        if time.ticks_diff(now, last_stat_ms) >= 10000:
            last_stat_ms = now
            peer_age = (time.ticks_diff(now, last_peer_ms) // 1000) if last_peer_ms else -1
            print("STAT tx=%d listen=%d fd=%d cmd=%d junk=%d lora=%d peer=%d age=%ds lat=%.5f lon=%.5f fix=%d sat=%d phase=%s t=%s" %
                  (tx_count, listen_count, 1 if fd_mode else 0, cmd_count, junk_stat[0], 1 if lora_ok else 0,
                   peer_count, peer_age,
                   lat_f, lon_f, 1 if gps_fix else 0, satellites, mission_phase, gps_time))
            # ★ 打印噪声样本：一眼就能看出收到的到底是真数据还是乱码
            if junk_stat[0] and junk_stat[1]:
                print("JUNK %d 行 样本=%s" % (junk_stat[0], junk_stat[1]))
                junk_stat[1] = []

        # ---- RGB 跟随阶段 ----
        #   ★ 放在主循环里每轮调用，是为了让"预备"阶段的灯能实时跟着
        #     GPS 定位状态变（未定位=红 → 定位成功=绿）。
        #   进入准备/点火/灭火/结束之后，rgb_apply_phase() 给的是该阶段的
        #   固定颜色，不再受 gps_fix 影响 —— 这就是主人要的优先级规则。
        #   rgb_set_color() 内部对相同颜色直接 return，所以每轮调开销极小。
        rgb_apply_phase()

        # 提示音降频，避免掩盖按键确认音
        # ★ 全部走非阻塞 play()：这两声原来会各堵住主循环几十到几百毫秒。
        if not gps_fix:
            gps_fix_played = False
            if time.ticks_diff(now, last_gps_beep_ms) >= 5000:
                last_gps_beep_ms = now
                buzzer_drv.play(((1047, 60, 0),))
        elif not gps_fix_played:
            gps_fix_played = True
            buzzer_drv.play(((1047, 150, 80), (1397, 150, 80), (1760, 220, 100)))
        # 链路提示音：开关关闭时**完全不参与**，避免任何误鸣。
        # （曾经写成 elif (not LINK_MONITOR) or ...，关闭状态下该分支每轮循环都为真，
        #   导致"链路正常"提示音无限重复——已修正为整体包在开关里。）
        if LINK_MONITOR:
            if not lora_ok and link_ever_ok:
                lora_ok_played = False
                # 只在"曾经通过、现在断了"时报警
                if time.ticks_diff(now, last_lora_beep_ms) >= 2000:
                    last_lora_beep_ms = now
                    buzzer_drv.play(((2600, 120, 80),))
            elif lora_ok and not lora_ok_played:
                lora_ok_played = True
                buzzer_drv.play(((523, 150, 80), (698, 150, 80), (880, 220, 100)))
        tick += 1
        time.sleep_ms(10)


if __name__ == "__main__": main()

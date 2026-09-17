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
try:
    import st7735
except Exception:
    st7735 = None
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
tft = None
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
last_peer_ms = 0           # 最近一次收到地面端有效回包的时刻
peer_count = 0             # 收到的地面端回包总数
tx_fail = False            # 上一次遥测写入是否抛异常
link_ever_ok = False       # 本次上电后是否曾经确认过链路

# ---- 板载 RGB 指示灯（WS2812 @ GPIO48）----
# 状态要求（最终确认）：
#   待命 / 结束            → 熄灭
#   确认 / 点火 / 飞行 / 灭火 → 全亮（白）
# 阶段来源与蜂鸣器一致：思澈端按键状态机下发 B:READY / B:ARM / B:IGNITE /
# B:END / B:ABORT。上电默认待命态，所以开机为灭。
# 注意 WS2812 掉电后状态随机且会锁存，初始化时必须主动写一次。
RGB_PIN = 48
RGB_ON_COLOR = (255, 255, 255)   # 全亮
RGB_OFF_COLOR = (0, 0, 0)
rgb_led = None
rgb_state = False


def rgb_set(on):
    """点亮/熄灭板载 RGB。写入失败不影响主逻辑。"""
    global rgb_state
    if rgb_led is None:
        return
    try:
        rgb_led[0] = RGB_ON_COLOR if on else RGB_OFF_COLOR
        rgb_led.write()
        rgb_state = on
    except Exception as e:
        print("RGB ERR", e)


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
    global mpu, bme, gps, lora, sd, sd_ok, tft, rgb_led
    if st7735:
        try:
            bl = machine.Pin(5, machine.Pin.OUT); bl.value(1)
            spi = machine.SPI(2, baudrate=20000000, polarity=0, phase=0,
                              sck=machine.Pin(12), mosi=machine.Pin(11))
            tft = st7735.ST7735(spi, machine.Pin(7, machine.Pin.OUT),
                                machine.Pin(46, machine.Pin.OUT), machine.Pin(10, machine.Pin.OUT),
                                width=160, height=128)
            tft.fill(0); tft.text("FLIGHT COMPUTER", 18, 45, 0xFFFF); tft.show()
            print("TFT OK")
        except Exception as e: print("TFT ERR", e)
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
    # 板载 RGB：上电即待命态 → 灭。WS2812 掉电后状态随机且会锁存，
    # 不做这一步开机可能一直乱亮。
    if neopixel is not None:
        try:
            rgb_led = neopixel.NeoPixel(machine.Pin(RGB_PIN), 1)
            rgb_set(False)
            print("RGB LED OK (standby: off)")
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
    global last_b_cmd, last_b_cmd_ms
    if line.startswith("B:"):
        body = line[2:].strip()
        now = time.ticks_ms()
        duplicate = (body == last_b_cmd and
                     time.ticks_diff(now, last_b_cmd_ms) < 2500)
        last_b_cmd = body
        last_b_cmd_ms = now
        if duplicate:
            send_ack(body)
            return True
        print("CMD RX B:" + body)
        if body == "READY":
            # 回到待命：上扬启动音，和确认/点火/灭火/结束区分
            for freq in (660, 880, 1175, 1568):
                buzzer_drv.beep(freq, 120, 55)
            rgb_set(False)                          # 待命 → RGB 灭
            send_ack("READY")
        elif body == "ARM":
            buzzer_drv.beep(440, 600, 120)          # 低音长鸣：点火确认
            rgb_set(True)                           # 确认 → RGB 亮
            send_ack("ARM")
        elif body == "IGNITE":
            for _ in range(5):
                buzzer_drv.beep(3200, 90, 55)       # 点火：5 声尖锐
            rgb_set(True)                           # 点火 → RGB 全亮
            send_ack("IGNITE")
        elif body == "END":
            for freq in (1200, 1000, 800, 600):
                buzzer_drv.beep(freq, 150, 55)       # 结束：4 声递降
            rgb_set(False)                          # 结束 → RGB 灭
            send_ack("END")
        elif body == "ABORT":
            for _ in range(2):
                buzzer_drv.beep(300, 300, 100)       # 灭火：2 声低沉
            rgb_set(True)                            # 灭火 → RGB 保持全亮
            send_ack("ABORT")
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
            for line in raw.replace("\r", "").split("\n"):
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
                handle_sd_cmd(line)
                got = True
                cmd_count += 1
    except Exception:
        pass
    return got



def play_melody(notes):
    for freq, duration, pause in notes:
        buzzer_drv.beep(freq, duration, pause)


def update_tft(pitch, roll, temp, pressure):
    if tft is None: return
    try:
        # ST7735 可视区 160x128。行位置沿用原来已验证安全的那一套
        # （y=8/25/42/59/76/93，末行 93+8=101，不顶到上下边缘）。
        # 之前试过排 8 行到 y=116，结果上下都顶格超出屏幕，已改回 6 行。
        # 版面：
        #   TELEMETRY
        #   G:FIX SAT16        ← 定位状态（原 LoRa 状态栏已从屏幕移除）
        #   ALT  123.4  V   4.5
        #   BAR 1013.2hPa      ← 气压（hPa，便于与气象数据对照）
        #   TMP   26.6C        ← 温度
        #   SD:1  12:34:56
        tft.fill(0)
        tft.text("TELEMETRY", 4, 8, 0xFFFF)
        tft.text("G:%s SAT%2d" % ("FIX" if gps_fix else "NO", satellites), 4, 25,
                 0x07E0 if gps_fix else 0xF800)
        tft.text("ALT %6.1fm V %4.1f" % (fused_alt, speed), 4, 42, 0x07FF)
        tft.text("BAR %7.1fhPa" % ((pressure or 0) / 100.0), 4, 59, 0xFDA0)
        tft.text("TMP %6.1fC" % (temp or 0), 4, 76, 0xFDA0)
        # SD 状态直接显示在屏幕上，一眼能看出"有没有卡、在不在录"
        if not sd_ok:
            sd_txt = "SD:-- NO CARD"
        elif sd_record:
            sd_txt = "SD:%d REC %s" % (sd_file_id, gps_time[:8] or "--:--:--")
        else:
            sd_txt = "SD:%d stop %s" % (sd_file_id, gps_time[:8] or "--:--:--")
        tft.text(sd_txt, 4, 93, 0x07E0 if sd_record else 0x8410)
        tft.show()
    except Exception as e: print("TFT update ERR", e)


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
    while True:
        # 按键命令最高优先级，循环 10ms 一次，最迟 10ms 内响应
        poll_lora_cmd()

        now = time.ticks_ms()

        # 命令优先：每 2 秒留 1 秒纯接收窗口。
        # LoRa 空中发射时间远长于 UART 写入时间，200ms 窗口不可靠；
        # 思澈会连续重发约 3.2 秒，确保 B:IGNITE 等命中窗口。
        # 窗口必须够长：模块空中发送时间远大于 UART 写入时间，
        # 400ms 时积压没排完窗口就结束，实测按键命令完全收不到。
        if listen_until == 0 and time.ticks_diff(now, last_listen_ms) >= 2000:
            listen_until = now + 1000
            last_listen_ms = now
            listen_count += 1
        in_listen = (listen_until != 0 and
                     time.ticks_diff(now, listen_until) < 0)
        if listen_until != 0 and not in_listen:
            listen_until = 0
        in_listen_now = in_listen      # 给 sd_flush() 用：窗口内不写卡

        # 只在"窗口刚结束"的那一刻补写一次（下降沿）。
        # 注意不能写成每轮都调——那样缓冲每轮被清空，攒批就完全失效了
        # （实测退化成每次只写 1 行、每行还要 1.1 秒）。
        if was_in_listen and not in_listen and sd_buf and sd_record:
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
                # 快包 250ms + 完整包 1500ms（约 4.3 包/秒）：
                # 实测这是这条 LoRa 链路的最优档（丢包约 8%，主要是
                # 为保证按键命令接收而留出的 10% 接收窗口），再快丢包会明显上升。
                # 250ms 是实测最优档：更快（180ms）链路饱和反而掉到 1.3 包/秒，
                # 这一档约 3.3 包/秒且不丢包。
                if time.ticks_diff(now, last_quick_ms) >= 250:
                    last_quick_ms = now
                    quick = {"p": round(pitch, 1), "r": round(roll, 1),
                             "y": round(yaw, 1)}
                    lora.write((json.dumps(quick, separators=(",", ":")) + "\n").encode())
                    tx_count += 1
                # 完整包：含加速度/气压/GPS，供思澈做融合计算
                if time.ticks_diff(now, last_full_ms) >= 2000:
                    last_full_ms = now
                    # 经纬度用于思澈端雷达：思澈本地算相对距离/方位，不回传、不额外占带宽
                    full = {"p": round(pitch, 1), "r": round(roll, 1),
                            "y": round(yaw, 1),
                            # 经纬度用 6 位小数（约 0.11m），避免截断掉 GPS 精度
                            "la": round(lat_f, 6), "lo": round(lon_f, 6),
                            "ax": round(a["x"], 3), "ay": round(a["y"], 3), "az": round(a["z"], 3),
                            "a": round(fused_alt, 1), "gps": 1 if gps_fix and satellites >= 4 else 0,
                            "s": satellites,
                            "t": round(temp, 1) if temp is not None else 0,
                            "P": round(pressure / 1000.0, 2) if pressure else 0,
                            "v": round(speed, 1), "T": gps_time}
                    # 链路字段只在开关打开时才上报（默认关闭，见 LINK_MONITOR 注释）
                    if LINK_MONITOR:
                        full["L"] = 1 if lora_ok else (-1 if not link_ever_ok else 0)
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
            print("STAT tx=%d listen=%d cmd=%d lora=%d peer=%d age=%ds lat=%.5f lon=%.5f fix=%d sat=%d t=%s" %
                  (tx_count, listen_count, cmd_count, 1 if lora_ok else 0,
                   peer_count, peer_age,
                   lat_f, lon_f, 1 if gps_fix else 0, satellites, gps_time))

        # 提示音降频，避免掩盖按键确认音
        if not gps_fix:
            gps_fix_played = False
            if time.ticks_diff(now, last_gps_beep_ms) >= 5000:
                last_gps_beep_ms = now
                buzzer_drv.beep(1047, 60, 0)
        elif not gps_fix_played:
            gps_fix_played = True
            play_melody(((1047, 150, 80), (1397, 150, 80), (1760, 220, 100)))
        # 链路提示音：开关关闭时**完全不参与**，避免任何误鸣。
        # （曾经写成 elif (not LINK_MONITOR) or ...，关闭状态下该分支每轮循环都为真，
        #   导致"链路正常"提示音无限重复——已修正为整体包在开关里。）
        if LINK_MONITOR:
            if not lora_ok and link_ever_ok:
                lora_ok_played = False
                # 只在"曾经通过、现在断了"时报警
                if time.ticks_diff(now, last_lora_beep_ms) >= 2000:
                    last_lora_beep_ms = now
                    buzzer_drv.beep(2600, 120, 80)
            elif lora_ok and not lora_ok_played:
                lora_ok_played = True
                play_melody(((523, 150, 80), (698, 150, 80), (880, 220, 100)))
        tick += 1
        time.sleep_ms(10)


if __name__ == "__main__": main()

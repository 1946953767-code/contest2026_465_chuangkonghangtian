"""
ESP32-S3 LoRa 传感器发射端 v2
- MPU6050 六轴 (I2C: SCL=GPIO9, SDA=GPIO8)
- ST7735 TFT 显示 (SPI)
- DX-LR22 LoRa 发射 (UART1: TX=GPIO17, RX=GPIO18)
"""
import machine
import time
import json
import struct
import math

# ===== UART1 -> LoRa (9600 8N1) =====
uart_lora = machine.UART(1, baudrate=9600, tx=17, rx=18)

# ===== I2C - MPU6050 =====
# 实测 MPU6050: SCL=GPIO9, SDA=GPIO3, 地址=0x68
i2c = machine.SoftI2C(scl=machine.Pin(9), sda=machine.Pin(3), freq=100000)
time.sleep_ms(200)

MPU_ADDR = 0x68
mpu_ok = False
try:
    i2c.writeto_mem(MPU_ADDR, 0x6B, b'\x00')  # wake up
    time.sleep_ms(100)
    whoami = i2c.readfrom_mem(MPU_ADDR, 0x75, 1)[0]
    print(f"MPU6050 WHO_AM_I = 0x{whoami:02X}")
    mpu_ok = True
except:
    try:
        MPU_ADDR = 0x69
        i2c.writeto_mem(MPU_ADDR, 0x6B, b'\x00')
        time.sleep_ms(100)
        whoami = i2c.readfrom_mem(MPU_ADDR, 0x75, 1)[0]
        print(f"MPU6050 @ 0x69 WHO_AM_I = 0x{whoami:02X}")
        mpu_ok = True
    except:
        print("MPU6050 not found!")

def read_mpu6050():
    """读取加速度和陀螺仪"""
    if not mpu_ok:
        return None
    try:
        data = i2c.readfrom_mem(MPU_ADDR, 0x3B, 14)
        ax = struct.unpack('>h', data[0:2])[0] / 16384.0
        ay = struct.unpack('>h', data[2:4])[0] / 16384.0
        az = struct.unpack('>h', data[4:6])[0] / 16384.0
        temp_raw = struct.unpack('>h', data[6:8])[0]
        temp = temp_raw / 340.0 + 36.53
        gx = struct.unpack('>h', data[8:10])[0] / 131.0
        gy = struct.unpack('>h', data[10:12])[0] / 131.0
        gz = struct.unpack('>h', data[12:14])[0] / 131.0
        pitch = math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az)))
        roll = math.degrees(math.atan2(ay, az))
        return {
            'ax': round(ax, 3), 'ay': round(ay, 3), 'az': round(az, 3),
            'gx': round(gx, 1), 'gy': round(gy, 1), 'gz': round(gz, 1),
            'pitch': round(pitch, 1), 'roll': round(roll, 1), 'yaw': 0.0,
            'temp': round(temp, 1)
        }
    except Exception as e:
        print(f"MPU read error: {e}")
        return None

# ===== SPI - ST7735 TFT =====
try:
    import st7735
    bl_pin = machine.Pin(5, machine.Pin.OUT)
    bl_pin.value(1)
    spi = machine.SPI(2, baudrate=20000000, polarity=0, phase=0,
                      sck=machine.Pin(12), mosi=machine.Pin(11))
    tft = st7735.ST7735(spi,
                        machine.Pin(7, machine.Pin.OUT),
                        machine.Pin(46, machine.Pin.OUT),
                        machine.Pin(10, machine.Pin.OUT),
                        width=160, height=128)
    tft.fill(0x0000)
    tft.text("LoRa TX Ready", 10, 10, 0xFFFF)
    tft.text("MPU6050 + TFT", 10, 30, 0x07E0)
    tft.show()
    tft_ok = True
    print("TFT OK")
except Exception as e:
    print("TFT init failed: %s" % e)
    tft_ok = False

C_BLACK = 0x0000
C_WHITE = 0xFFFF
C_RED = 0xF800
C_GREEN = 0x07E0
C_BLUE = 0x001F
C_YELLOW = 0xFFE0
C_CYAN = 0x07FF

def draw_bar(x, y, w, h, value, max_val, color):
    if tft_ok:
        tft.fill_rect(x, y, w, h, 0x2104)
        bar_w = int(abs(value) / max_val * w)
        if bar_w > w: bar_w = w
        if bar_w > 0:
            if value >= 0:
                tft.fill_rect(x, y, bar_w, h, color)
            else:
                tft.fill_rect(x + w - bar_w, y, bar_w, h, color)

def update_tft(data, pkt_id):
    if not tft_ok or data is None:
        return
    try:
        tft.fill(C_BLACK)
        # 标题栏 (160x128 横屏布局)
        tft.fill_rect(0, 0, 160, 14, C_BLUE)
        tft.text("Rocket TX", 4, 3, C_WHITE)
        tft.text("P%d" % pkt_id, 110, 3, C_YELLOW)

        # 加速度 (左侧)
        tft.text("ACC", 4, 18, C_CYAN)
        tft.text("X:%+.2f" % data['ax'], 4, 28, C_RED)
        tft.text("Y:%+.2f" % data['ay'], 4, 40, C_GREEN)
        tft.text("Z:%+.2f" % data['az'], 4, 52, C_BLUE)

        # 加速度仪表条 (中间)
        draw_bar(64, 28, 40, 6, data['ax'], 2.0, C_RED)
        draw_bar(64, 40, 40, 6, data['ay'], 2.0, C_GREEN)
        draw_bar(64, 52, 40, 6, data['az'] - 1.0, 2.0, C_BLUE)

        # 陀螺仪 (右侧)
        tft.text("GYR", 112, 18, C_YELLOW)
        tft.text("X:%+.0f" % data['gx'], 112, 28, C_RED)
        tft.text("Y:%+.0f" % data['gy'], 112, 40, C_GREEN)
        tft.text("Z:%+.0f" % data['gz'], 112, 52, C_BLUE)

        # 分隔线
        tft.fill_rect(0, 64, 160, 1, C_WHITE)

        # 温度 + 状态
        tft.text("TEMP: %.1fC" % data['temp'], 4, 70, C_WHITE)
        tft.text("LoRa 433MHz", 4, 84, C_GREEN)
        tft.text("TX OK", 4, 96, C_GREEN)

        # 底部状态栏
        tft.fill_rect(0, 114, 160, 14, C_BLUE)
        tft.text("6-axis telemetry", 20, 117, C_WHITE)

        tft.show()
    except Exception as e:
        print("TFT error: %s" % e)

# ===== 主循环 =====
pkt_id = 0
print("\n=== LoRa TX v2 Ready ===\n")

while True:
    data = read_mpu6050()
    if data:
        pkt = {
            "id": pkt_id,
            "ax": data['ax'], "ay": data['ay'], "az": data['az'],
            "gx": data['gx'], "gy": data['gy'], "gz": data['gz'],
            "pitch": data['pitch'], "roll": data['roll'], "yaw": data['yaw'],
            "t": data['temp']
        }
        # 只发送火箭姿态所需字段，保持 JSON 兼容并显著缩短空口包。
        # 当前 yaw 没有磁力计，省略无效字段，只传实时俯仰/横滚。
        msg = json.dumps({
            "p": data['pitch'], "r": data['roll']
        }, separators=(',', ':')) + "\n"
        uart_lora.write(msg.encode())
        # TFT 全屏刷屏较慢，降低本地显示频率，不影响 LoRa 发送。
        if pkt_id % 10 == 0:
            update_tft(data, pkt_id)
        if pkt_id % 50 == 0:
            print("[%d] AX=%+.3f AY=%+.3f AZ=%+.3f GX=%+.1f GY=%+.1f GZ=%+.1f T=%.1fC" %
                  (pkt_id, data['ax'], data['ay'], data['az'],
                   data['gx'], data['gy'], data['gz'], data['temp']))
        pkt_id += 1
    else:
        print("No MPU6050 data!")
    time.sleep_ms(100)

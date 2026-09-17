from pathlib import Path
import shutil
import sys


TAA_FILE = Path("taa.py")
AHT20_FILE = Path("aht20.py")


AHT20_DRIVER = r'''import time


class AHT20:
    ADDRESS = 0x38

    def __init__(self, i2c, address=0x38):
        self.i2c = i2c
        self.address = address
        self.present = False

        try:
            if self.address not in self.i2c.scan():
                print("AHT20 address not found:", hex(self.address))
                return

            time.sleep_ms(100)
            self._init_sensor()
            self.present = True
            print("AHT20 OK")
        except Exception as e:
            print("AHT20 error:", e)
            self.present = False

    def _read_status(self):
        return self.i2c.readfrom(self.address, 1)[0]

    def _init_sensor(self):
        status = self._read_status()

        # Calibration enable bit
        if not (status & 0x08):
            self.i2c.writeto(
                self.address,
                bytes((0xBE, 0x08, 0x00))
            )
            time.sleep_ms(20)

    def read(self):
        """
        Return:
            temperature: Celsius
            humidity: relative humidity percentage
        """
        if not self.present:
            return None, None

        # Start measurement
        self.i2c.writeto(
            self.address,
            bytes((0xAC, 0x33, 0x00))
        )

        data = None

        # Wait up to about 100 ms
        for _ in range(10):
            time.sleep_ms(10)
            data = self.i2c.readfrom(self.address, 7)

            # Status bit 7: measurement busy
            if not (data[0] & 0x80):
                break

        if data is None or (data[0] & 0x80):
            raise OSError("AHT20 measurement timeout")

        # 20-bit humidity raw value
        raw_humidity = (
            (data[1] << 12) |
            (data[2] << 4) |
            (data[3] >> 4)
        )

        # 20-bit temperature raw value
        raw_temperature = (
            ((data[3] & 0x0F) << 16) |
            (data[4] << 8) |
            data[5]
        )

        humidity = raw_humidity * 100.0 / 1048576.0
        temperature = raw_temperature * 200.0 / 1048576.0 - 50.0

        if humidity < 0:
            humidity = 0
        if humidity > 100:
            humidity = 100

        return temperature, humidity
'''


def fail(message):
    print("[ERROR]", message)
    sys.exit(1)


if not TAA_FILE.exists():
    fail("找不到 taa.py，请把补丁脚本放到 taa.py 同一目录。")


# 备份原文件，只保留第一份备份
backup_file = Path("taa.py.before_aht20")
if not backup_file.exists():
    shutil.copyfile(TAA_FILE, backup_file)
    print("[OK] 已备份为 taa.py.before_aht20")

with open(TAA_FILE, "r", encoding="utf-8") as f:
    code = f.read()

changes = []

# ============================================================
# 1. 添加 AHT20 导入
# ============================================================
if "from aht20 import AHT20" not in code:
    import_marker = "from drv import BME280, MAX30102"

    if import_marker not in code:
        fail("找不到 from drv import BME280, MAX30102")

    code = code.replace(
        import_marker,
        import_marker + "\nfrom aht20 import AHT20",
        1
    )
    changes.append("添加 AHT20 导入")
else:
    print("[SKIP] AHT20 导入已经存在")


# ============================================================
# 2. 添加模块级全局变量
# ============================================================
old_global_line = (
    "tft=oled=i2c=i2c2=bme=max30102=mpu=np=led_pin=None"
)

new_global_line = (
    "tft=oled=i2c=i2c2=bme=aht20=max30102=mpu=np=led_pin=None"
)

if "bme=aht20=max30102" not in code:
    if old_global_line not in code:
        fail("找不到 taa.py 的模块级硬件全局变量行")

    code = code.replace(
        old_global_line,
        new_global_line,
        1
    )
    changes.append("添加全局变量 aht20")
else:
    print("[SKIP] 全局变量 aht20 已存在")


# ============================================================
# 3. 在 init_hw() 的 global 声明中添加 aht20
# ============================================================
old_init_global = (
    "global tft, oled, i2c, i2c2, bme, max30102, mpu, np, led_pin"
)

new_init_global = (
    "global tft, oled, i2c, i2c2, bme, aht20, max30102, mpu, np, led_pin"
)

if "bme, aht20, max30102" not in code:
    if old_init_global not in code:
        fail("找不到 init_hw() 的 global 硬件声明")

    code = code.replace(
        old_init_global,
        new_init_global,
        1
    )
    changes.append("在 init_hw() 中声明 aht20")
else:
    print("[SKIP] init_hw() 已声明 aht20")


# ============================================================
# 4. 在 i2c2 初始化后加入 AHT20 初始化
# ============================================================
aht20_init_block = '''    try:
        aht20 = AHT20(i2c2)
        print("AHT20 present:", aht20.present)
    except Exception as e:
        print("AHT20 init error:", e)
        aht20 = None
'''

compact_i2c2 = (
    "    i2c2=SoftI2C(scl=Pin(12),sda=Pin(13),freq=400000)"
)

formatted_i2c2 = '''    i2c2 = SoftI2C(
        scl=Pin(12),
        sda=Pin(13),
        freq=400000
    )'''

if "aht20 = AHT20(i2c2)" not in code:
    if compact_i2c2 in code:
        code = code.replace(
            compact_i2c2,
            compact_i2c2 + "\n" + aht20_init_block.rstrip(),
            1
        )
        changes.append("在紧凑格式 i2c2 后初始化 AHT20")

    elif formatted_i2c2 in code:
        code = code.replace(
            formatted_i2c2,
            formatted_i2c2 + "\n\n" + aht20_init_block.rstrip(),
            1
        )
        changes.append("在格式化 i2c2 后初始化 AHT20")

    else:
        fail("找不到 i2c2 = SoftI2C(GPIO12, GPIO13) 初始化位置")
else:
    print("[SKIP] AHT20 初始化代码已经存在")


# ============================================================
# 5. 写回 taa.py
# ============================================================
with open(TAA_FILE, "w", encoding="utf-8", newline="") as f:
    f.write(code)

print("[OK] taa.py 补丁完成")
for item in changes:
    print("  -", item)


# ============================================================
# 6. 生成 aht20.py
# ============================================================
if AHT20_FILE.exists():
    backup_driver = Path("aht20.py.before_patch")
    if not backup_driver.exists():
        shutil.copyfile(AHT20_FILE, backup_driver)
        print("[OK] 原 aht20.py 已备份为 aht20.py.before_patch")

with open(AHT20_FILE, "w", encoding="utf-8", newline="") as f:
    f.write(AHT20_DRIVER)

print("[OK] 已生成正确的 aht20.py")
print()
print("接下来上传这两个文件到 ESP32-S3：")
print("  1. taa.py")
print("  2. aht20.py")
print()
print("注意：gpspage.py 还必须是会读取 ctx.aht20 的版本。")

# ============================================================
# telemetry.py —— 遥测发射端 (ESP32-S3 AP 热点 + TCP 服务器)
# 功能：ESP32-S3 开热点 → 传感器读取 → TCP 广播数据
# 用法：在 REPL 里运行 import telemetry; telemetry.run()
#       或者改 main.py BOOT_DELAY=0 后自动加载
# ============================================================
import gc, network, socket, time, json, math
from machine import Pin, SoftI2C, UART, SPI
import ssd1306
import st7735
from drv import BME280
from aht20 import AHT20

# ---- 热点参数 ----
AP_SSID = "ROCKET_TELEM"
AP_PASS = "12345678"
AP_CHANNEL = 6
TCP_PORT = 8080

# ---- 传感器引脚（和 taa.py 一致） ----
TFT_SCL, TFT_SDA, TFT_RES, TFT_DC, TFT_CS, TFT_BL = 18, 8, 39, 38, 37, 40
GPS_RX_PIN = 2
RGB_PIN = 48

# ---- 全局 ----
tft = oled = bme = aht20 = mpu = gps = np = None
pkt_id = 0

def init_hw():
    global tft, oled, bme, aht20, mpu, gps, np
    gc.collect()
    # TFT
    bl = Pin(TFT_BL, Pin.OUT); bl.value(1)
    spi = SPI(1, baudrate=20000000, polarity=0, phase=0,
              sck=Pin(TFT_SCL), mosi=Pin(TFT_SDA))
    tft = st7735.ST7735(spi, Pin(TFT_CS, Pin.OUT), Pin(TFT_DC, Pin.OUT),
                        Pin(TFT_RES, Pin.OUT), width=160, height=128)
    tft.fill(0); tft.text("TELEMETRY TX", 30, 60, 0xFFFF); tft.show()
    # OLED
    i2c1 = SoftI2C(scl=Pin(16), sda=Pin(17), freq=400000)
    oled = ssd1306.SSD1306_I2C(128, 64, i2c1)
    # 传感器 (I2C2: SCL=12, SDA=13)
    i2c2 = SoftI2C(scl=Pin(12), sda=Pin(13), freq=400000)
    try: bme = BME280(i2c2); print("BME280:", hex(bme.chip_id) if bme.present else "N/A")
    except Exception as e: print("BME err:", e); bme = None
    try: aht20 = AHT20(i2c2); print("AHT20:", aht20.present)
    except Exception as e: print("AHT err:", e); aht20 = None
    try:
        from mpu6050 import MPU6050
        mpu = MPU6050(i2c1); print("MPU6050 OK")
    except Exception as e: print("MPU err:", e); mpu = None
    # GPS
    try:
        gps = UART(1, baudrate=9600, rx=Pin(GPS_RX_PIN), timeout=200, rxbuf=1024)
        print("GPS UART OK")
    except Exception as e: print("GPS err:", e); gps = None
    # RGB LED
    try:
        from neopixel import NeoPixel
        np = NeoPixel(Pin(RGB_PIN, Pin.OUT), 1)
        np[0] = (0, 0, 255); np.write()
    except: np = None
    gc.collect()
    print("HW init done, free:", gc.mem_free())

def read_sensors():
    data = {}
    # BME280
    if bme and bme.present:
        try:
            t, p, h = bme.read()
            data["temp"] = round(t, 1)
            data["press"] = round(p / 100.0, 1)
            data["hum"] = round(h, 1)
            data["alt"] = round(44330.0 * (1.0 - (p / 101325.0) ** (1.0 / 5.255)), 1)
        except: pass
    # AHT20
    if aht20 and aht20.present and "temp" not in data:
        try:
            t, h = aht20.read()
            if t is not None: data["temp"] = round(t, 1)
            if h is not None: data["hum"] = round(h, 1)
        except: pass
    # MPU6050
    if mpu:
        try:
            a = mpu.read_accel_data()
            data["ax"] = round(a["x"], 3)
            data["ay"] = round(a["y"], 3)
            data["az"] = round(a["z"], 3)
        except: pass
    return data

def parse_gps(buf):
    info = {}
    for line in buf.split('\n'):
        p = line.strip().split(',')
        if line.startswith(('$GNGGA', '$GPGGA')) and len(p) >= 10:
            info["sv"] = int(p[7]) if p[7].isdigit() else 0
            if p[6] in ('1', '2') and p[9]:
                try: info["galt"] = float(p[9])
                except: pass
        if line.startswith(('$GNRMC', '$GPRMC')) and len(p) >= 10:
            if p[2] == 'A' and len(p) > 7:
                try:
                    lat_raw = p[3]; lon_raw = p[5]
                    d, m = int(lat_raw[:2]), float(lat_raw[2:])
                    ns = p[4]
                    info["lat"] = d + m / 60.0
                    if ns == 'S': info["lat"] = -info["lat"]
                    d, m = int(lon_raw[:3]), float(lon_raw[3:])
                    ew = p[6]
                    info["lon"] = d + m / 60.0
                    if ew == 'W': info["lon"] = -info["lon"]
                    info["spd"] = round(float(p[7]) * 0.514444, 2)
                except: pass
    return info

def start_ap():
    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    ap.config(ssid=AP_SSID, password=AP_PASS, channel=AP_CHANNEL)
    while not ap.active():
        time.sleep(0.1)
    ip = ap.ifconfig()[0]
    print("AP ready:", AP_SSID, "IP:", ip)
    return ip

def serve(clients, ip_addr):
    global pkt_id
    gps_buf = ""
    last_disp = 0

    while True:
        # GPS 读取
        if gps and gps.any():
            try:
                raw = gps.read(gps.any())
                gps_buf += raw.decode('utf-8', 'ignore')
            except: gps_buf = ""
            while '\n' in gps_buf:
                line, gps_buf = gps_buf.split('\n', 1)

        # 传感器数据
        data = read_sensors()
        if gps:
            gps_info = parse_gps(gps_buf)
            data.update(gps_info)
        data["id"] = pkt_id
        data["t"] = time.ticks_ms()
        pkt_id += 1

        line = json.dumps(data, separators=(',', ':')) + "\n"
        lb = line.encode('utf-8')

        # 广播给所有客户端
        dead = []
        for cl in clients:
            try:
                cl.send(lb)
            except:
                dead.append(cl)
        for cl in dead:
            try: cl.close()
            except: pass
            clients.remove(cl)

        # 显示更新
        now = time.ticks_ms()
        if time.ticks_diff(now, last_disp) >= 500:
            last_disp = now
            cnt = len(clients)
            temp = data.get("temp", "--")
            alt = data.get("alt", "--")
            sv = data.get("sv", 0)
            # OLED
            try:
                oled.fill(0)
                oled.text("TX: " + AP_SSID, 0, 0)
                oled.text("IP:" + ip_addr, 0, 12)
                oled.text("Clients:" + str(cnt), 0, 24)
                oled.text("T:{}C A:{}m".format(temp, alt), 0, 36)
                oled.text("SV:{} PKT:{}".format(sv, pkt_id), 0, 48)
                oled.text("GRP:" + data.get("press", "--"), 0, 56)
                oled.show()
            except: pass
            # TFT
            try:
                tft.fill(0)
                tft.fill_rect(0, 0, 160, 16, 0x001F)
                tft.text("TELEMETRY TX", 30, 4, 0xFFFF)
                tft.text("AP:" + AP_SSID, 4, 22, 0x07FF)
                tft.text("Clients:" + str(cnt), 4, 34, 0x07E0 if cnt else 0xF800)
                tft.text("T:{}C H:{}%".format(
                    data.get("temp", "--"), data.get("hum", "--")), 4, 50, 0xFFFF)
                tft.text("P:{}hPa".format(data.get("press", "--")), 4, 62, 0xFFFF)
                tft.text("Alt:{}m".format(data.get("alt", "--")), 4, 74, 0xFFE0)
                tft.text("SV:{} SPD:{}".format(
                    data.get("sv", 0), data.get("spd", "--")), 4, 86, 0x07E0)
                tft.text("PKT:" + str(pkt_id), 4, 100, 0x8410)
                tft.show()
            except: pass
            # RGB LED 状态
            if np:
                np[0] = (0, 255, 0) if cnt else (255, 165, 0)
                np.write()

        # 接受新连接
        try:
            cl, addr = srv.accept()
            cl.settimeout(0.01)
            clients.append(cl)
            print("Client connected:", addr)
        except OSError:
            pass

        time.sleep_ms(100)

def run():
    """启动遥测发射端"""
    print("=== Telemetry TX ===")
    gc.collect()
    init_hw()
    gc.collect()
    ip = start_ap()

    global srv
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', TCP_PORT))
    srv.listen(3)
    srv.settimeout(0.01)
    print("TCP server on port", TCP_PORT)

    clients = []
    try:
        serve(clients, ip)
    except KeyboardInterrupt:
        print("Stopped")
    finally:
        for cl in clients:
            try: cl.close()
            except: pass
        srv.close()

if __name__ == "__main__":
    run()

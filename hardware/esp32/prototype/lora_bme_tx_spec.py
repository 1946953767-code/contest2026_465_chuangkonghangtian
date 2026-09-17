import machine
import time
import json
from drv import BME280

# ESP32S3 sensor hub final pin map
TFT_SCK, TFT_MOSI = 18, 8
TFT_RES, TFT_DC, TFT_CS, TFT_BL = 39, 38, 37, 40
BME_SCL, BME_SDA = 12, 13
LORA_TX, LORA_RX = 17, 16

BLACK, WHITE, GREEN, CYAN, YELLOW, RED = 0x0000, 0xFFFF, 0x07E0, 0x07FF, 0xFFE0, 0xF800

uart = machine.UART(1, baudrate=9600, tx=LORA_TX, rx=LORA_RX)
bl = machine.Pin(TFT_BL, machine.Pin.OUT)
bl.value(1)

tft = None
try:
    import st7735
    spi = machine.SPI(1, baudrate=20000000, polarity=0, phase=0,
                      sck=machine.Pin(TFT_SCK), mosi=machine.Pin(TFT_MOSI))
    tft = st7735.ST7735(spi, machine.Pin(TFT_CS, machine.Pin.OUT),
                        machine.Pin(TFT_DC, machine.Pin.OUT),
                        machine.Pin(TFT_RES, machine.Pin.OUT), width=160, height=128)
    print("TFT OK: 18/8")
except Exception as e:
    print("TFT ERR:", e)

bme = None
try:
    i2c = machine.SoftI2C(scl=machine.Pin(BME_SCL), sda=machine.Pin(BME_SDA), freq=100000)
    print("I2C2 scan:", [hex(x) for x in i2c.scan()])
    bme = BME280(i2c)
    print("BME280:", hex(bme.chip_id) if bme.present else "N/A")
except Exception as e:
    print("BME ERR:", e)


def draw(seq, data):
    if tft is None:
        return
    tft.fill(BLACK)
    tft.fill_rect(0, 0, 160, 16, 0x001F)
    tft.text("BME280 + LoRa", 18, 4, WHITE)
    tft.text("ID:%d" % seq, 4, 24, YELLOW)
    if data is None:
        tft.text("BME N/A", 4, 50, RED)
        tft.text("I2C 12/13", 4, 68, RED)
    else:
        tft.text("P:%.1f hPa" % data["press"], 4, 45, CYAN)
        tft.text("ALT:%.1f m" % data["alt"], 4, 63, GREEN)
        tft.text("T:%.1f C" % data["temp"], 4, 81, WHITE)
        tft.text("H:%.1f %%" % data["hum"], 4, 97, WHITE)
    tft.text("TX 9600", 52, 116, GREEN)
    tft.show()


def read_bme():
    if bme is None or not bme.present:
        return None
    try:
        temp, pressure, hum = bme.read()
        return {"press": round(pressure / 100.0, 1),
                "alt": round(44330.0 * (1.0 - (pressure / 101325.0) ** (1.0 / 5.255)), 1),
                "temp": round(temp, 1), "hum": round(hum, 1)}
    except Exception as e:
        print("BME READ ERR:", e)
        return None

seq = 0
print("=== BME LoRa TX SPEC ===")
while True:
    data = read_bme()
    packet = {"id": seq, "press": data["press"] if data else None,
              "alt": data["alt"] if data else None,
              "temp": data["temp"] if data else None,
              "hum": data["hum"] if data else None}
    uart.write((json.dumps(packet) + "\n").encode())
    draw(seq, data)
    print("TX", packet)
    seq += 1
    time.sleep(1)

"""ESP32 LoRa + BME280 test using the existing TFT/LoRa wiring."""
import machine
import time
import json
from drv import BME280

# Existing verified wiring from lora_tx.py
TFT_SCK = 12
TFT_MOSI = 11
TFT_CS = 7
TFT_DC = 46
TFT_RES = 10
TFT_BL = 5
LORA_TX = 17
LORA_RX = 18
BME_SCL = 12
BME_SDA = 13

C_BLACK = 0x0000
C_WHITE = 0xFFFF
C_GREEN = 0x07E0
C_CYAN = 0x07FF
C_YELLOW = 0xFFE0
C_RED = 0xF800

uart_lora = machine.UART(1, baudrate=9600, tx=LORA_TX, rx=LORA_RX)
bl = machine.Pin(TFT_BL, machine.Pin.OUT)
bl.value(1)

tft = None
spi = None
try:
    import st7735
    spi = machine.SPI(2, baudrate=20000000, polarity=0, phase=0,
                      sck=machine.Pin(TFT_SCK), mosi=machine.Pin(TFT_MOSI))
    tft = st7735.ST7735(spi, machine.Pin(TFT_CS, machine.Pin.OUT),
                         machine.Pin(TFT_DC, machine.Pin.OUT),
                         machine.Pin(TFT_RES, machine.Pin.OUT),
                         width=160, height=128)
    print("TFT OK")
except Exception as exc:
    print("TFT error:", exc)

bme = None
try:
    # The sensor bus shares TFT_SCK; release/reclaim it around each BME read.
    bus = machine.SoftI2C(scl=machine.Pin(BME_SCL), sda=machine.Pin(BME_SDA),
                          freq=100000)
    bme = BME280(bus)
    print("BME280:", hex(bme.chip_id) if bme.present else "N/A")
except Exception as exc:
    print("BME error:", exc)


def restore_tft():
    global spi, tft
    if tft is None:
        return
    try:
        spi = machine.SPI(2, baudrate=20000000, polarity=0, phase=0,
                          sck=machine.Pin(TFT_SCK), mosi=machine.Pin(TFT_MOSI))
    except Exception:
        pass


def read_bme():
    if bme is None or not bme.present:
        return None
    try:
        # Re-create the I2C bus because the shared SCK pin is returned to SPI.
        bus = machine.SoftI2C(scl=machine.Pin(BME_SCL), sda=machine.Pin(BME_SDA),
                              freq=100000)
        bme.i2c = bus
        temp, pressure, humidity = bme.read()
        restore_tft()
        return {
            "temp": round(temp, 1),
            "press": round(pressure / 100.0, 1),
            "hum": round(humidity, 1),
            "alt": round(44330.0 * (1.0 - (pressure / 101325.0) ** (1.0 / 5.255)), 1)
        }
    except Exception as exc:
        print("BME read error:", exc)
        restore_tft()
        return None


def show(data, seq):
    if tft is None:
        return
    try:
        tft.fill(C_BLACK)
        tft.fill_rect(0, 0, 160, 16, 0x001F)
        tft.text("LORA BME TX", 22, 4, C_WHITE)
        tft.text("ID:%d" % seq, 4, 24, C_YELLOW)
        if data is None:
            tft.text("BME N/A", 4, 48, C_RED)
            tft.text("CHECK I2C", 4, 66, C_RED)
        else:
            tft.text("P:%.1f hPa" % data["press"], 4, 44, C_CYAN)
            tft.text("ALT:%.1f m" % data["alt"], 4, 62, C_GREEN)
            tft.text("T:%.1f C H:%.1f%%" % (data["temp"], data["hum"]), 4, 80, C_WHITE)
        tft.text("LoRa 9600", 4, 106, C_GREEN)
        tft.show()
    except Exception as exc:
        print("TFT draw error:", exc)


print("=== LoRa BME TX ===")
seq = 0
while True:
    data = read_bme()
    packet = {"id": seq, "press": data["press"] if data else None,
              "alt": data["alt"] if data else None,
              "temp": data["temp"] if data else None,
              "hum": data["hum"] if data else None}
    uart_lora.write((json.dumps(packet) + "\n").encode())
    show(data, seq)
    print("TX", packet)
    seq += 1
    time.sleep(1)

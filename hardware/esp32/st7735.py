from machine import Pin, SPI
import time
import framebuf

class ST7735(framebuf.FrameBuffer):
    def __init__(self, spi, cs, dc, res, width=160, height=128):
        self.spi = spi
        self.cs = cs
        self.dc = dc
        self.res = res
        self.width = width
        self.height = height
        self.buffer = bytearray(width * height * 2)
        super().__init__(self.buffer, width, height, framebuf.RGB565)
        self.cs.value(1)
        self.dc.value(1)
        self.res.value(1)
        self.init_display()

    def reset(self):
        self.res.value(0)
        time.sleep(0.05)
        self.res.value(1)
        time.sleep(0.15)

    def write_cmd(self, cmd):
        self.cs.value(0); self.dc.value(0)
        self.spi.write(bytearray([cmd]))
        self.cs.value(1)

    def write_data(self, data):
        self.cs.value(0); self.dc.value(1)
        self.spi.write(bytearray(data))
        self.cs.value(1)

    def init_display(self):
        self.reset()
        self.write_cmd(0x01); time.sleep(0.15)
        self.write_cmd(0x11); time.sleep(0.5)
        self.write_cmd(0xB1); self.write_data([0x01,0x2C,0x2D])
        self.write_cmd(0xB2); self.write_data([0x01,0x2C,0x2D])
        self.write_cmd(0xB3); self.write_data([0x01,0x2C,0x2D,0x01,0x2C,0x2D])
        self.write_cmd(0xB4); self.write_data([0x07])
        self.write_cmd(0xC0); self.write_data([0xA2,0x02,0x84])
        self.write_cmd(0xC1); self.write_data([0xC5])
        self.write_cmd(0xC2); self.write_data([0x0A,0x00])
        self.write_cmd(0xC3); self.write_data([0x8A,0x2A])
        self.write_cmd(0xC4); self.write_data([0x8A,0xEE])
        self.write_cmd(0xC5); self.write_data([0x0E])
        self.write_cmd(0x20)
        # 横屏 + RGB（若颜色反了改 0x68）
        self.write_cmd(0x36); self.write_data([0x60])
        self.write_cmd(0x3A); self.write_data([0x05])
        self.write_cmd(0x2A); self.write_data([0x00,0x00,0x00,self.width-1])
        self.write_cmd(0x2B); self.write_data([0x00,0x00,0x00,self.height-1])
        self.write_cmd(0xE0); self.write_data([0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10])
        self.write_cmd(0xE1); self.write_data([0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10])
        self.write_cmd(0x13); time.sleep(0.01)
        self.write_cmd(0x29); time.sleep(0.1)

    def show(self):
        self.write_cmd(0x2A); self.write_data([0x00,0x00,0x00,self.width-1])
        self.write_cmd(0x2B); self.write_data([0x00,0x00,0x00,self.height-1])
        self.write_cmd(0x2C)
        self.cs.value(0); self.dc.value(1)
        self.spi.write(self.buffer)
        self.cs.value(1)
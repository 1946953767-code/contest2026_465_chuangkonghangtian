"""AHT20 温湿度传感器驱动（I2C，默认地址 0x38）

AHT20 是"初始化一次、每次测量等 80 ms"的时序型器件，
所以这里刻意做成"非阻塞"：trigger() 发起测量，read() 取回结果，
主循环可以先干别的，不用原地 sleep。

用法：
    bus = machine.SoftI2C(scl=Pin(13), sda=Pin(12), freq=100000)
    aht = AHT20(bus)            # 会自动做一次初始化（若已初始化则跳过）
    aht.trigger()               # 发起一次测量
    ...                         # 主循环继续跑，别处干 80 ms 的活
    t, h = aht.read()           # 取回（未就绪返回 (None, None)）

寄存器：0x71 状态位；0xAC 0x33 0x00 = 触发测量；0xBE 0x08 0x00 = 初始化
"""

import time


class AHT20:
    ADDR = 0x38
    CMD_STATUS = 0x71
    CMD_INIT = b'\xbe\x08\x00'
    CMD_TRIGGER = b'\xac\x33\x00'
    CAL_READY = 0x18        # bit3=1 已校准, bit4=1 未忙

    def __init__(self, i2c, addr=0x38):
        self.i2c = i2c
        self.addr = addr
        self.present = False
        self._t = None
        self._h = None
        self._t0 = 0
        self._busy = False
        try:
            st = i2c.readfrom_mem(addr, self.CMD_STATUS, 1)[0]
            self.present = True
            if (st & self.CAL_READY) != self.CAL_READY:
                # 未校准：发初始化命令，等 10 ms 后重读状态
                try:
                    i2c.writeto(addr, self.CMD_INIT)
                    time.sleep_ms(10)
                    st = i2c.readfrom_mem(addr, self.CMD_STATUS, 1)[0]
                    self.present = (st & self.CAL_READY) == self.CAL_READY
                except Exception:
                    self.present = False
            self.status = st
            # ★ 首次测量丢弃：AHT20 上电后第一帧常不可靠（内部校准未完成），
            #   直接发出去会看到第一行温度明显偏差。这里阻塞 90ms 采一次扔掉。
            try:
                i2c.writeto(addr, self.CMD_TRIGGER)
                time.sleep_ms(95)
                i2c.readfrom(addr, 7)
            except Exception:
                pass
        except Exception:
            self.present = False

    # ---- 非阻塞：发起测量 ----
    def trigger(self):
        if not self.present or self._busy:
            return False
        try:
            self.i2c.writeto(self.addr, self.CMD_TRIGGER)
            self._busy = True
            self._t0 = time.ticks_ms()
            return True
        except Exception:
            self.present = False
            return False

    # ---- 取结果：未就绪返回 (None, None) ----
    def read(self):
        if not self.present:
            return (None, None)
        if not self._busy:
            # 没有正在进行的测量 -> 直接读上次结果（首帧就会返回 None）
            return (self._t, self._h)
        if time.ticks_diff(time.ticks_ms(), self._t0) < 80:
            return (self._t, self._h)
        try:
            d = self.i2c.readfrom(self.addr, 7)
            self._busy = False
            if (d[0] & 0x80):        # 忙位仍为 1
                self._busy = True
                return (self._t, self._h)
            raw_h = (d[1] << 12) | (d[2] << 4) | (d[3] >> 4)
            raw_t = ((d[3] & 0x0F) << 16) | (d[4] << 8) | d[5]
            self._h = raw_h * 100.0 / 1048576.0
            self._t = raw_t * 200.0 / 1048576.0 - 50.0
            return (self._t, self._h)
        except Exception:
            self._busy = False
            return (self._t, self._h)

    # ---- 阻塞版（测试/一次性读取用）----
    def read_blocking(self):
        self.trigger()
        time.sleep_ms(90)
        return self.read()
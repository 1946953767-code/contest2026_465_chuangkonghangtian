# ============================================================
# drv.py —— 传感器驱动（BME280 气压/温度/湿度 + MAX30102 心率/血氧）
# 拆成独立文件是为了减小 taa.py 体积：ESP32-S3 在 WiFi 连接后
# 内存有限，太大的 .py 编译时会 "Out of Memory"。
# ============================================================
import time

# ============================================================
# BME280 / BMP280 驱动（I2C 0x76 或 0x77）—— 气压 / 温度 / 湿度
# ============================================================
class BME280:
    def __init__(self, i2c, addr=0x76):
        self.i2c = i2c
        self.addr = addr
        self.chip_id = 0
        self.present = False
        self.dig = {}
        self.t_fine = 0
        try:
            self.chip_id = i2c.readfrom_mem(addr, 0xD0, 1)[0]
        except Exception:
            try:
                self.addr = 0x77
                self.chip_id = i2c.readfrom_mem(0x77, 0xD0, 1)[0]
            except Exception:
                self.chip_id = 0
        if self.chip_id in (0x60, 0x58):   # BME280 / BMP280
            self.present = True
            self._load_calib()
            try:
                self.i2c.writeto_mem(self.addr, 0xF2, b'\x01')  # 湿度过采样 x1
                self.i2c.writeto_mem(self.addr, 0xF4, b'\x4B')  # 温度/气压过采样 x2 + 正常模式
                self.i2c.writeto_mem(self.addr, 0xF5, b'\x10')  # IIR 滤波 x4（气压更稳）
            except Exception:
                pass

    @staticmethod
    def _signed(v, bits):
        if v & (1 << (bits - 1)):
            v -= (1 << bits)
        return v

    def _load_calib(self):
        d = self.i2c.readfrom_mem(self.addr, 0x88, 26)
        dig = self.dig
        dig['T1'] = d[0] | (d[1] << 8)
        dig['T2'] = self._signed(d[2] | (d[3] << 8), 16)
        dig['T3'] = self._signed(d[4] | (d[5] << 8), 16)
        dig['P1'] = d[6] | (d[7] << 8)
        dig['P2'] = self._signed(d[8] | (d[9] << 8), 16)
        dig['P3'] = self._signed(d[10] | (d[11] << 8), 16)
        dig['P4'] = self._signed(d[12] | (d[13] << 8), 16)
        dig['P5'] = self._signed(d[14] | (d[15] << 8), 16)
        dig['P6'] = self._signed(d[16] | (d[17] << 8), 16)
        dig['P7'] = self._signed(d[18] | (d[19] << 8), 16)
        dig['P8'] = self._signed(d[20] | (d[21] << 8), 16)
        dig['P9'] = self._signed(d[22] | (d[23] << 8), 16)
        if self.chip_id == 0x60:   # BME280 才有湿度校准（BMP280 没有）
            dig['H1'] = self.i2c.readfrom_mem(self.addr, 0xA1, 1)[0]
            e = self.i2c.readfrom_mem(self.addr, 0xE1, 7)
            dig['H2'] = self._signed(e[0] | (e[1] << 8), 16)
            dig['H3'] = e[2]
            dig['H4'] = self._signed((e[3] << 4) | (e[4] & 0x0F), 12)
            dig['H5'] = self._signed((e[4] >> 4) | (e[5] << 4), 12)
            dig['H6'] = self._signed(e[6], 8)

    def read(self):
        """返回 (温度°C, 气压Pa, 湿度%)"""
        data = self.i2c.readfrom_mem(self.addr, 0xF7, 8)
        adc_p = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4)
        adc_t = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4)
        adc_h = (data[6] << 8) | data[7]
        d = self.dig
        # ---- 温度（0.01°C）----
        var1 = (((adc_t >> 3) - (d['T1'] << 1)) * d['T2']) >> 11
        var2 = (((((adc_t >> 4) - d['T1']) * ((adc_t >> 4) - d['T1'])) >> 12) * d['T3']) >> 14
        self.t_fine = var1 + var2
        temp = (self.t_fine * 5 + 128) >> 8
        # ---- 气压（BOSCH 浮点公式，输出 Pa，已验证）----
        var1 = (self.t_fine / 2.0) - 64000.0
        var2 = var1 * var1 * (d['P6'] / 32768.0)
        var2 = var2 + (var1 * d['P5'] * 2.0)
        var2 = (var2 / 4.0) + (d['P4'] * 65536.0)
        var1 = ((d['P3'] * var1 * var1) / 524288.0 + (d['P2'] * var1)) / 524288.0
        var1 = (1.0 + var1 / 32768.0) * d['P1']
        if var1 == 0.0:
            press = 0.0
        else:
            press = 1048576.0 - adc_p
            press = (press - (var2 / 4096.0)) * 6250.0 / var1
            var1 = (d['P9'] * press * press) / 2147483648.0
            var2 = press * (d['P8'] / 32768.0)
            press = press + (var1 + var2 + d['P7']) / 16.0
        # ---- 湿度（Q22.10 → %）----
        hum = 0.0
        if self.chip_id == 0x60:
            v = self.t_fine - 76800
            v = (((((adc_h << 14) - (d['H4'] << 20) - (d['H5'] * v)) + 16384) >> 15) *
                 (((((((v * d['H6']) >> 10) * (((v * d['H3']) >> 11) + 32768)) >> 10) + 2097152) * d['H2'] + 8192) >> 14))
            v = v - (((((v >> 15) * (v >> 15)) >> 7) * d['H1']) >> 4)
            if v < 0: v = 0
            if v > 419430400: v = 419430400
            hum = (v >> 12) / 1024.0
        return temp / 100.0, press, hum

# ============================================================
# MAX30102 驱动（I2C 0x57）—— 心率 / 血氧
# ============================================================
class MAX30102:
    REG_INTR1       = 0x00
    REG_INTR2       = 0x01
    REG_INTR_EN1    = 0x02
    REG_INTR_EN2    = 0x03
    REG_FIFO_WR_PTR = 0x04
    REG_OVF_COUNTER = 0x05
    REG_FIFO_RD_PTR = 0x06
    REG_FIFO_DATA   = 0x07
    REG_FIFO_CFG    = 0x08
    REG_MODE_CFG    = 0x09
    REG_SPO2_CFG    = 0x0A
    REG_LED1_PA     = 0x0B   # RED
    REG_LED2_PA     = 0x0C   # IR
    REG_LED3_PA     = 0x0D
    REG_PART_ID     = 0xFF

    def __init__(self, i2c, addr=0x57):
        self.i2c = i2c
        self.addr = addr
        self.present = False
        try:
            pid = i2c.readfrom_mem(addr, self.REG_PART_ID, 1)[0]
            self.present = (pid == 0x15)   # MAX30102 的器件号
        except Exception:
            pass
        if self.present:
            try:
                self._w(self.REG_MODE_CFG, 0x40)   # 复位
                time.sleep_ms(100)
                self._w(self.REG_INTR_EN1, 0x00)   # 关中断（轮询 FIFO）
                self._w(self.REG_INTR_EN2, 0x00)
                self._w(self.REG_FIFO_WR_PTR, 0x00)
                self._w(self.REG_OVF_COUNTER, 0x00)
                self._w(self.REG_FIFO_RD_PTR, 0x00)
            except Exception:
                pass

    def _w(self, reg, val):
        self.i2c.writeto_mem(self.addr, reg, bytes([val]))

    def set_pa(self, red, ir):
        try:
            self._w(self.REG_LED1_PA, red)
            self._w(self.REG_LED2_PA, ir)
        except Exception:
            pass

    def enable(self, red_pa=0x26, ir_pa=0x26):
        """开启采样：SpO2 模式，ADC 100Hz，2 次平均 → FIFO ~50Hz"""
        try:
            self._w(self.REG_MODE_CFG, 0x40)       # 复位
            time.sleep_ms(50)
            self._w(self.REG_FIFO_CFG, 0x30)       # SMP_AVE=2, FIFO 卷回
            self._w(self.REG_SPO2_CFG, 0x27)       # 4096nA, 100Hz, 411us
            self._w(self.REG_LED1_PA, red_pa)
            self._w(self.REG_LED2_PA, ir_pa)
            self._w(self.REG_FIFO_WR_PTR, 0x00)
            self._w(self.REG_OVF_COUNTER, 0x00)
            self._w(self.REG_FIFO_RD_PTR, 0x00)
            self._w(self.REG_MODE_CFG, 0x03)       # SpO2 模式 → 开始采样
        except Exception:
            pass

    def disable(self):
        """关灯停采（省电）"""
        try:
            self._w(self.REG_LED1_PA, 0x00)
            self._w(self.REG_LED2_PA, 0x00)
            self._w(self.REG_MODE_CFG, 0x02)
        except Exception:
            pass

    def check(self):
        """读 FIFO 中所有新样本，返回 [(red, ir), ...]，值都是 18bit"""
        out = []
        try:
            wr = self.i2c.readfrom_mem(self.addr, self.REG_FIFO_WR_PTR, 1)[0] & 0x1F
            rd = self.i2c.readfrom_mem(self.addr, self.REG_FIFO_RD_PTR, 1)[0] & 0x1F
            n = (wr - rd) & 0x1F
            if n == 0:
                return out
            data = self.i2c.readfrom_mem(self.addr, self.REG_FIFO_DATA, n * 6)
            # 部分克隆芯片不会自动推进读指针，手动同步一下
            try:
                self._w(self.REG_FIFO_RD_PTR, (rd + n) & 0x1F)
            except Exception:
                pass
            for i in range(n):
                off = i * 6
                red = (data[off] << 16 | data[off + 1] << 8 | data[off + 2]) & 0x7FFFF
                ir  = (data[off + 3] << 16 | data[off + 4] << 8 | data[off + 5]) & 0x7FFFF
                out.append((red, ir))
        except Exception:
            pass
        return out

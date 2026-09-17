import time

class MPU6050:
    ADDR = 0x68
    REG_ACCEL_XOUT_H = 0x3B
    REG_PWR_MGMT_1   = 0x6B
    REG_GYRO_CONFIG  = 0x1B
    REG_ACCEL_CONFIG = 0x1C

    def __init__(self, i2c, addr=0x68):
        self.i2c = i2c
        self.addr = addr
        try:
            self.i2c.writeto_mem(addr, self.REG_PWR_MGMT_1, b'\x00')
            time.sleep_ms(50)
            # 加速度计 ±2g，陀螺仪 ±250°/s（默认）
            self.i2c.writeto_mem(addr, self.REG_ACCEL_CONFIG, b'\x00')
            self.i2c.writeto_mem(addr, self.REG_GYRO_CONFIG, b'\x00')
        except Exception:
            pass

    def read_raw(self, reg, nbytes):
        return self.i2c.readfrom_mem(self.addr, reg, nbytes)

    def _s16(self, b, off):
        v = (b[off] << 8) | b[off+1]
        if v & 0x8000:
            v -= 65536
        return v

    def read_accel_data(self):
        d = self.read_raw(self.REG_ACCEL_XOUT_H, 14)
        ax = self._s16(d, 0) / 16384.0
        ay = self._s16(d, 2) / 16384.0
        az = self._s16(d, 4) / 16384.0
        return {'x': ax, 'y': ay, 'z': az}

    def read_gyro_data(self):
        d = self.read_raw(self.REG_ACCEL_XOUT_H + 8, 6)
        gx = self._s16(d, 0) / 131.0
        gy = self._s16(d, 2) / 131.0
        gz = self._s16(d, 4) / 131.0
        return {'x': gx, 'y': gy, 'z': gz}
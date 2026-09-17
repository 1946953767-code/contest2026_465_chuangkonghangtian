import time


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

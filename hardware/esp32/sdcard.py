"""
MicroPython SDCard block device driver, SPI mode.
适用于大多数 SPI 方式的 TF / Micro SD 模块
"""

from micropython import const
import time

_CMD_TIMEOUT = const(100)

_R1_IDLE_STATE = const(1 << 0)
_R1_ILLEGAL_COMMAND = const(1 << 2)

_TOKEN_CMD25 = const(0xFC)
_TOKEN_STOP_TRAN = const(0xFD)
_TOKEN_DATA = const(0xFE)


class SDCard:
    def __init__(self, spi, cs, baudrate=1320000):
        self.spi = spi
        self.cs = cs

        self.cs.init(self.cs.OUT, value=1)

        # 先低速初始化
        self.init_card(baudrate)

    def init_spi(self, baudrate):
        try:
            self.spi.init(baudrate=baudrate, phase=0, polarity=0)
        except TypeError:
            self.spi.init(baudrate=baudrate)

    def init_card(self, baudrate):
        self.init_spi(100000)

        # 至少 74 个时钟，CS 拉高
        self.cs(1)
        for _ in range(16):
            self.spi.write(b"\xff")

        # CMD0: reset
        for _ in range(5):
            if self.cmd(0, 0, 0x95) == _R1_IDLE_STATE:
                break
        else:
            raise OSError("no SD card")

        # CMD8: determine card version
        r = self.cmd(8, 0x01AA, 0x87, 4)
        if r == _R1_IDLE_STATE:
            self.init_card_v2()
        elif r & _R1_ILLEGAL_COMMAND:
            self.init_card_v1()
        else:
            raise OSError("couldn't determine SD card version")

        # 读取 CSD
        # 注意 release=False：CMD9 之后卡会紧跟着发 CSD 数据块，
        # 若此处拉高 CS，卡会中止数据块发送，后面的 readinto 就永远等不到
        # 0xFE 起始令牌（表现为 "timeout waiting for response"，卡在初始化）。
        # readblocks() 里的 CMD17/18 原本就带了 release=False，这里是对齐它。
        if self.cmd(9, 0, 0, 0, False) != 0:
            self.cs(1)
            raise OSError("no response from SD card")
        csd = bytearray(16)
        self.readinto(csd)
        self.sectors = self.parse_csd(csd)

        # 切到高速
        self.init_spi(baudrate)

    def init_card_v1(self):
        # 同 init_card_v2：把超时从 5 秒压到 0.6 秒，避免卡没插好时白等。
        for _ in range(24):
            time.sleep_ms(25)
            self.cmd(55, 0, 0)
            if self.cmd(41, 0, 0) == 0:
                self.cdv = 512
                return
        raise OSError("timeout waiting for v1 card")

    def init_card_v2(self):
        # 超时上限从 100×50ms（5 秒）压到 24×25ms（0.6 秒）。
        # SD 初始化本来就是毫秒级的事；卡没插好时原来的 5 秒纯属白等，
        # 而上层还会重试多次 —— 实测 6 次重试能把开机拖住 30 秒以上，
        # 表现为"ESP32 卡住了"。压到 0.6 秒后 6 次重试总共也只占 3.6 秒。
        for _ in range(24):
            time.sleep_ms(25)
            self.cmd(58, 0, 0, 4)
            self.cmd(55, 0, 0)
            if self.cmd(41, 0x40000000, 0) == 0:
                # v2 卡还要看 OCR 的 CCS 位决定寻址方式：
                #   CCS=1 → SDHC/SDXC，按块寻址（cdv=1）
                #   CCS=0 → SDSC（标准容量，如 1GB 卡），必须按字节寻址（cdv=512）
                # 原实现一律按块寻址。对 SDSC 卡，0 号块碰巧两种寻址都是 0
                # 所以"看起来能读"，但读 1 号块就错，表现为挂载时报 read error。
                self.cs(0)
                self.spi.read(1, 0xFF)
                self.spi.write(bytes([0x40 | 58, 0, 0, 0, 0, 0]))
                for _ in range(_CMD_TIMEOUT):
                    resp = self.spi.read(1, 0xFF)[0]
                    if not (resp & 0x80):
                        break
                ocr = self.spi.read(4, 0xFF)
                self.cs(1)
                self.spi.write(b"\xff")
                self.cdv = 1 if (ocr[0] & 0x40) else 512
                return
        raise OSError("timeout waiting for v2 card")

    def cmd(self, cmd, arg, crc, final=0, release=True, skip1=False):
        self.cs(0)

        if not skip1:
            self.spi.read(1, 0xFF)

        buf = bytearray(6)
        buf[0] = 0x40 | cmd
        buf[1] = arg >> 24
        buf[2] = arg >> 16
        buf[3] = arg >> 8
        buf[4] = arg
        buf[5] = crc
        self.spi.write(buf)

        if cmd == 12:
            self.spi.read(1, 0xFF)

        for _ in range(_CMD_TIMEOUT):
            response = self.spi.read(1, 0xFF)[0]
            if not (response & 0x80):
                if final:
                    self.spi.readinto(bytearray(final), 0xFF)
                if release:
                    self.cs(1)
                    self.spi.write(b"\xff")
                return response

        self.cs(1)
        self.spi.write(b"\xff")
        return -1

    def readinto(self, buf):
        self.cs(0)

        for _ in range(_CMD_TIMEOUT):
            token = self.spi.read(1, 0xFF)[0]
            if token == _TOKEN_DATA:
                break
            time.sleep_ms(1)
        else:
            self.cs(1)
            self.spi.write(b"\xff")
            raise OSError("timeout waiting for response")

        self.spi.readinto(buf, 0xFF)

        # 丢掉 CRC
        self.spi.read(2, 0xFF)

        self.cs(1)
        self.spi.write(b"\xff")

    def write(self, token, buf):
        self.cs(0)
        self.spi.read(1, 0xFF)
        self.spi.write(bytes([token]))
        self.spi.write(buf)
        self.spi.write(b"\xff")
        self.spi.write(b"\xff")

        response = self.spi.read(1, 0xFF)[0]
        if (response & 0x1F) != 0x05:
            self.cs(1)
            self.spi.write(b"\xff")
            raise OSError("write error")

        # 等卡结束内部编程（DO 拉高）。
        # 原实现是无上限忙等 `while ... == 0: pass`：卡一旦进入坏状态就永远
        # 出不来，直接把整个主循环拖死（实测表现为 ESP32 完全无串口输出、
        # LoRa 收发全停、屏幕不再刷新）。这里加超时上限，超时抛错，
        # 由上层统计连续失败次数并关掉 SD 记录，保住主循环。
        _n = 0
        while self.spi.read(1, 0xFF)[0] == 0:
            _n += 1
            if _n > 40000:          # 400kHz 下约 0.8 秒
                self.cs(1)
                self.spi.write(b"\xff")
                raise OSError("write timeout")

        self.cs(1)
        self.spi.write(b"\xff")

    def write_token(self, token):
        self.cs(0)
        self.spi.read(1, 0xFF)
        self.spi.write(bytes([token]))
        self.spi.write(b"\xff")
        _n = 0
        while self.spi.read(1, 0xFF)[0] == 0:
            _n += 1
            if _n > 40000:              # 同上：不能让忙等无上限
                self.cs(1)
                self.spi.write(b"\xff")
                raise OSError("write timeout")
        self.cs(1)
        self.spi.write(b"\xff")

    def readblocks(self, block_num, buf):
        nblocks = len(buf) // 512
        if nblocks == 1:
            if self.cmd(17, block_num * self.cdv, 0, release=False) != 0:
                self.cs(1)
                raise OSError("read error")
            self.readinto(buf)
        else:
            if self.cmd(18, block_num * self.cdv, 0, release=False) != 0:
                self.cs(1)
                raise OSError("read error")
            offset = 0
            while nblocks:
                self.readinto(memoryview(buf)[offset:offset + 512])
                offset += 512
                nblocks -= 1
            if self.cmd(12, 0, 0xFF, skip1=True):
                raise OSError("stop read error")

    def writeblocks(self, block_num, buf):
        nblocks = len(buf) // 512
        if nblocks == 1:
            if self.cmd(24, block_num * self.cdv, 0) != 0:
                raise OSError("write error")
            self.write(_TOKEN_DATA, buf)
        else:
            if self.cmd(25, block_num * self.cdv, 0) != 0:
                raise OSError("write error")
            offset = 0
            while nblocks:
                self.write(_TOKEN_CMD25, memoryview(buf)[offset:offset + 512])
                offset += 512
                nblocks -= 1
            self.write_token(_TOKEN_STOP_TRAN)

    def ioctl(self, op, arg):
        if op == 4:
            return self.sectors
        if op == 5:
            return 512
        if op == 6:
            return 0

    def parse_csd(self, csd):
        if (csd[0] & 0xC0) == 0x40:
            c_size = ((csd[7] & 0x3F) << 16) | (csd[8] << 8) | csd[9]
            return (c_size + 1) * 1024
        else:
            read_bl_len = csd[5] & 0x0F
            c_size = ((csd[6] & 0x03) << 10) | (csd[7] << 2) | ((csd[8] & 0xC0) >> 6)
            c_size_mult = ((csd[9] & 0x03) << 1) | ((csd[10] & 0x80) >> 7)
            block_len = 1 << read_bl_len
            mult = 1 << (c_size_mult + 2)
            blocknr = (c_size + 1) * mult
            capacity = blocknr * block_len
            return capacity // 512

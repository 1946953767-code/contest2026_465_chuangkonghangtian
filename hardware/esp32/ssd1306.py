# MicroPython SSD1306 OLED driver, I2C and SPI interfaces

from micropython import const
import framebuf


# register definitions
SET_CONTRAST = const(0x81)
SET_ENTIRE_ON = const(0xA4)
SET_NORM_INV = const(0xA6)
SET_DISP = const(0xAE)
SET_MEM_ADDR = const(0x20)
SET_COL_ADDR = const(0x21)
SET_PAGE_ADDR = const(0x22)
SET_DISP_START_LINE = const(0x40)
SET_SEG_REMAP = const(0xA0)
SET_MUX_RATIO = const(0xA8)
SET_IREF_SELECT = const(0xAD)
SET_COM_OUT_DIR = const(0xC0)
SET_DISP_OFFSET = const(0xD3)
SET_COM_PIN_CFG = const(0xDA)
SET_DISP_CLK_DIV = const(0xD5)
SET_PRECHARGE = const(0xD9)
SET_VCOM_DESEL = const(0xDB)
SET_CHARGE_PUMP = const(0x8D)


# Subclassing FrameBuffer provides support for graphics primitives
# http://docs.micropython.org/en/latest/pyboard/library/framebuf.html
class SSD1306(framebuf.FrameBuffer):
    def __init__(self, width, height, external_vcc):
        self.width = width
        self.height = height
        self.external_vcc = external_vcc
        self.pages = self.height // 8
        self.buffer = bytearray(self.pages * self.width)
        super().__init__(self.buffer, self.width, self.height, framebuf.MONO_VLSB)
        self.init_display()

    def init_display(self):
        for cmd in (
            SET_DISP,  # display off
            # address setting
            SET_MEM_ADDR,
            0x00,  # horizontal
            # resolution and layout
            SET_DISP_START_LINE,  # start at line 0
            SET_SEG_REMAP | 0x01,  # column addr 127 mapped to SEG0
            SET_MUX_RATIO,
            self.height - 1,
            SET_COM_OUT_DIR | 0x08,  # scan from COM[N] to COM0
            SET_DISP_OFFSET,
            0x00,
            SET_COM_PIN_CFG,
            0x02 if self.width > 2 * self.height else 0x12,
            # timing and driving scheme
            SET_DISP_CLK_DIV,
            0x80,
            SET_PRECHARGE,
            0x22 if self.external_vcc else 0xF1,
            SET_VCOM_DESEL,
            0x30,  # 0.83*Vcc
            # display
            SET_CONTRAST,
            0xFF,  # maximum
            SET_ENTIRE_ON,  # output follows RAM contents
            SET_NORM_INV,  # not inverted
            SET_IREF_SELECT,
            0x30,  # enable internal IREF during display on
            # charge pump
            SET_CHARGE_PUMP,
            0x10 if self.external_vcc else 0x14,
            SET_DISP | 0x01,  # display on
        ):  # on
            self.write_cmd(cmd)
        self.fill(0)
        self.show()

    def poweroff(self):
        self.write_cmd(SET_DISP)

    def poweron(self):
        self.write_cmd(SET_DISP | 0x01)

    def contrast(self, contrast):
        self.write_cmd(SET_CONTRAST)
        self.write_cmd(contrast)

    def invert(self, invert):
        self.write_cmd(SET_NORM_INV | (invert & 1))

    def rotate(self, rotate):
        self.write_cmd(SET_COM_OUT_DIR | ((rotate & 1) << 3))
        self.write_cmd(SET_SEG_REMAP | (rotate & 1))

    def show(self):
        x0 = 0
        x1 = self.width - 1
        if self.width != 128:
            # narrow displays use centred columns
            col_offset = (128 - self.width) // 2
            x0 += col_offset
            x1 += col_offset
        self.write_cmd(SET_COL_ADDR)
        self.write_cmd(x0)
        self.write_cmd(x1)
        self.write_cmd(SET_PAGE_ADDR)
        self.write_cmd(0)
        self.write_cmd(self.pages - 1)
        self.write_data(self.buffer)


class SSD1306_I2C(SSD1306):
    def __init__(self, width, height, i2c, addr=0x3C, external_vcc=False):
        self.i2c = i2c
        self.addr = addr
        self.temp = bytearray(2)
        self.write_list = [b"\x40", None]  # Co=0, D/C#=1
        super().__init__(width, height, external_vcc)
        # 整个显示缓冲的零拷贝视图（show_diff 用，只建这一次）
        self._mv = memoryview(self.buffer)
        # 影子缓冲：记录"屏幕上现在是什么"。差异刷新就靠它比对。
        # 初始化成和缓冲区逐字节取反，保证第一帧一定会整屏发一遍。
        self._prev = bytearray(len(self.buffer))
        for i in range(len(self.buffer)):
            self._prev[i] = self.buffer[i] ^ 0xFF

    def write_cmd(self, cmd):
        self.temp[0] = 0x80  # Co=1, D/C#=0
        self.temp[1] = cmd
        self.i2c.writeto(self.addr, self.temp)

    def write_data(self, buf):
        self.write_list[1] = buf
        self.i2c.writevto(self.addr, self.write_list)

    # ================================================================
    # ★ 本项目加的方法：分页刷新（比官方 show() 稳得多）
    #
    # 为什么不用官方那个 show()：
    #   官方 show() 是"水平寻址 + 一次性把 1024 字节全发出去"。
    #   本项目实测：程序跑起来之后（WiFi 在工作、传感器在轮询），
    #   这么长一串 I2C 数据很容易被中断打断，
    #   一旦错位就是**整帧错乱** —— 屏幕上一会儿冒出莫名其妙的字符
    #   （实测看到过"大大的 4"、"横着的 3"），
    #   一会儿上一页的内容和新页叠在一起（看起来就是"页面重叠"）。
    #
    #   改成"页寻址、一次只发 128 字节"之后：
    #     · 每发完一页都重新设置一次页/列地址，相当于不断**重新同步**；
    #     · 万一某一页被打断，坏的也只有那一行（8 行里的一行），
    #       下一帧自己就刷回来了，不会整屏糊掉。
    #
    #   实测对照（tools/_addrscan2.py）：
    #     方式 A 一次性发 1024 字节 → 会乱
    #     方式 B 每次发 128 字节     → 正常
    #
    #   代价：慢一点（8 次事务 vs 1 次），
    #   但页面 800ms 才刷一次，实测一帧 60~130ms，绰绰有余。
    # ================================================================
    def show_paged(self):
        """按页刷新（每页一个 I2C 事务）—— 备用的整屏刷新方法。

        ⚠️ 注意：本项目主程序现在用的是下面的 **show_diff()**（差异刷新），
           不是这个方法。这个方法保留着给诊断脚本用。
        """
        self.write_cmd(SET_MEM_ADDR)
        self.write_cmd(0x02)      # 页寻址模式
        mv = self._mv
        w = self.width
        for page in range(self.pages):
            self.i2c.writeto(self.addr,
                             bytes((0x00, 0xB0 | page, 0x00, 0x10)))
            self.write_data(mv[page * w:(page + 1) * w])

    # ================================================================
    # ★★ show_diff() —— 差异刷新（本项目主程序实际使用的方法）★★
    #
    #  问题背景：
    #    官方 show() 每帧向屏幕推 1024 字节（整屏）。传输量越大，
    #    被 WiFi 中断打断 / 受电源波动影响的机会就越多，
    #    屏幕上就会出现"内容重叠、出现乱码"。
    #
    #  解决思路：
    #    实际上每帧只有极少数内容在变（GPS 秒数、高度小数点、
    #    气压末位），1024 字节里真正变化的往往只有十来个字节。
    #    所以：
    #      · 每帧把新缓冲和上一帧**逐页比较**；
    #      · 只把**变化了的那一页**（128 字节）发出去；
    #      · 完全没变的帧**一个字节都不发**。
    #
    #  好处：
    #    1. 传输量降 8~∞ 倍 → 被打断的机会同比降低；
    #    2. 没变化的区域永远不会被重写 → **画面绝对不会闪烁或重叠**；
    #    3. I2C 总线空出来了，传感器采样和旋钮响应更及时。
    #
    #  代价：
    #    切页时 8 页全变，第一次要发 1024 字节（和官方 show 一样）；
    #    之后就基本只发 1~2 页了。
    # ================================================================
    def show_diff(self):
        """只把**变化了的页**发出去。返回实际发送的页数。"""
        mv = self._mv
        w = self.width
        prev = self._prev
        cur = self.buffer
        sent = 0

        for page in range(self.pages):
            a = page * w
            b = a + w
            if prev[a:b] == cur[a:b]:
                continue                      # 这一页没变，跳过（一个字节都不发）
            self.write_cmd(SET_MEM_ADDR)
            self.write_cmd(0x02)              # 页寻址模式
            self.i2c.writeto(self.addr,
                             bytes((0x00, 0xB0 | page, 0x00, 0x10)))
            self.write_data(mv[a:b])
            prev[a:b] = cur[a:b]              # 更新影子缓冲
            sent += 1

        return sent

    def invalidate(self):
        """把影子缓冲清成"和屏幕完全不同"，强制下一帧整屏重发。

        什么时候用：
          · 刚开机（屏幕上是黑的，缓冲区内容未知）
          · 切页之后（内容全变，本来就会整屏发）
          · 从诊断脚本接管屏幕之后（屏幕内容已不可知）
        """
        p = self._prev
        c = self.buffer
        # 设成和当前缓冲区"逐字节取反"，保证一定不相等
        for i in range(len(p)):
            p[i] = c[i] ^ 0xFF


class SSD1306_SPI(SSD1306):
    def __init__(self, width, height, spi, dc, res, cs, external_vcc=False):
        self.rate = 10 * 1024 * 1024
        dc.init(dc.OUT, value=0)
        res.init(res.OUT, value=0)
        cs.init(cs.OUT, value=1)
        self.spi = spi
        self.dc = dc
        self.res = res
        self.cs = cs
        import time

        self.res(1)
        time.sleep_ms(1)
        self.res(0)
        time.sleep_ms(10)
        self.res(1)
        super().__init__(width, height, external_vcc)

    def write_cmd(self, cmd):
        self.spi.init(baudrate=self.rate, polarity=0, phase=0)
        self.cs(1)
        self.dc(0)
        self.cs(0)
        self.spi.write(bytearray([cmd]))
        self.cs(1)

    def write_data(self, buf):
        self.spi.init(baudrate=self.rate, polarity=0, phase=0)
        self.cs(1)
        self.dc(1)
        self.cs(0)
        self.spi.write(buf)
        self.cs(1)

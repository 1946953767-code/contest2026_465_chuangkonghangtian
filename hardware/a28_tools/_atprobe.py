# -*- coding: utf-8 -*-
"""找出 A28 到底认什么输入：穷举各种形态，看有没有哪一种不回 ERROR。"""
import machine, time

CASES = [
    (b"\r\n",                  "空行"),
    (b"\x00",                  "单字节0x00"),
    (b"+++",                   "+++ 转义"),
    (b"+++",                   "+++ 二次"),
    (b"AT\r",                  "AT+CR"),
    (b"AT\n",                  "AT+LF"),
    (b"at\r\n",                "小写at"),
    (b"AT \r\n",               "AT+空格"),
    (b"ATZ\r\n",               "ATZ 重启"),
    (b"\xaf\xaf\x73\x00\xaf\xf3", "0xAF二进制帧(RSSI)"),
    (b"\xaf\xaf",              "0xAF 0xAF"),
    (b"AT+ADDR?\r\n",          "AT+ADDR?"),
    (b"AT+MODE?\r\n",          "AT+MODE?"),
    (b"AT+INFO\r\n",           "AT+INFO"),
    (b"AT+SN\r\n",             "AT+SN"),
    (b"AT+CSQ\r\n",            "AT+CSQ"),
    (b"AT+FVER\r\n",           "AT+FVER"),
    (b"AT+BAND\r\n",           "AT+BAND"),
    (b"AT+UART?\r\n",          "AT+UART?"),
    (b"AT+RATE\r\n",           "AT+RATE"),
    (b"AT+AirRate\r\n",        "AT+AirRate"),
    (b"AT+ADDRESS\r\n",        "AT+ADDRESS"),
    (b"AT+CHANNEL?\r\n",       "AT+CHANNEL?"),
    (b"AT+NETID?\r\n",         "AT+NETID?"),
    (b"AT+VER=?\r\n",          "AT+VER=?"),
    (b"HELP\r\n",              "裸HELP"),
    (b"AT?",                   "AT?"),
    (b"AT-?\r\n",              "AT-?"),
]

u = machine.UART(1, baudrate=9600, tx=17, rx=18, timeout=200, rxbuf=2048)
time.sleep_ms(200); u.read()

def drain(secs):
    t0 = time.ticks_ms(); got = b""
    while time.ticks_diff(time.ticks_ms(), t0) < secs * 1000:
        r = u.read()
        if r: got += r
        time.sleep_ms(15)
    return got

print("=== 穷举输入形态（9600）===")
diff = []
for data, name in CASES:
    drain(0.35)
    u.write(data)
    got = drain(1.2)
    g = got.strip()
    mark = ""
    if g and g != b"ERROR":
        mark = "  <<<<< 非 ERROR 响应！"
        diff.append((name, g[:50]))
    print("  %-22s -> %r%s" % (name, g[:40], mark))

print()
print("=== 有 %d 种输入没回 ERROR ===" % len(diff))
for n, v in diff:
    print("  %s -> %r" % (n, v))
print("=== 完 ===")

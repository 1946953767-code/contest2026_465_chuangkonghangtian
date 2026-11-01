# -*- coding: utf-8 -*-
"""A28 二进制配置协议：读版本号 + 读当前配置参数。
模块当前处于【休眠配置工作状态】(MD0=1,MD1=1)，9600 8N1，正是配置模式。"""
import machine, time

u = machine.UART(1, baudrate=9600, tx=17, rx=18, timeout=200, rxbuf=2048)
time.sleep_ms(300)
u.read()

def drain(secs):
    t0 = time.ticks_ms(); got = b""
    while time.ticks_diff(time.ticks_ms(), t0) < secs * 1000:
        r = u.read()
        if r: got += r
        time.sleep_ms(15)
    return got

CMDS = [
    (b"\xc3\xc3\xc3", "读硬件版本号"),
    (b"\xf3\xf3\xf3", "读软件版本号"),
    (b"\xc1\xc1\xc1", "读配置参数"),
]

print("=== A28 二进制配置协议探测 ===")
for cmd, name in CMDS:
    drain(0.3)
    u.write(cmd)
    got = drain(1.8)
    print("  %-12s 发 %s" % (name, cmd.hex(' ').upper()))
    print("               收 %s" % (got.hex(' ').upper() if got else "(空)"))
    print("               文本 %r" % got)
    if len(got) >= 6 and got[0] == 0xC0:
        p = got[1:6]
        print("               └ 解析: ADDH=0x%02X ADDL=0x%02X SPEED=0x%02X CHAN=0x%02X OPTION=0x%02X"
              % (p[0], p[1], p[2], p[3], p[4]))
        cs = (p[2] >> 6) & 3
        bd = (p[2] >> 3) & 7
        ai = p[2] & 7
        bn = ["1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200"][bd]
        print("               └ 校验=%s  波特率=%s  空速档=%d" % (["8N1", "8O1", "8E1", "8N1"][cs], bn, ai))
        print("               └ 地址=0x%02X%02X  信道=%d  OPTION=0x%02X" % (p[0], p[1], p[3], p[4]))
print("=== 完 ===")

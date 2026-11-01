# -*- coding: utf-8 -*-
"""A28 自适应波特率假设验证：
如果模块是自适应的，它必须先收到同步字节才能测出主机波特率。
0x55 = 01010101 是最标准的测速波形。"""
import machine, time

SYNCS = [(b"\x55" * 40, "0x55x40"), (b"\xaa" * 40, "0xAAx40"), (b"U" * 40, "'U'x40")]
BAUDS = [115200, 57600, 38400, 19200, 9600]

print("=== 同步头 + 自适应波特率 验证 ===")
for baud in BAUDS:
    u = machine.UART(1, baudrate=baud, tx=17, rx=18, timeout=250, rxbuf=2048)
    time.sleep_ms(200)
    u.read()
    for sync, sname in SYNCS:
        u.read()
        u.write(sync)
        time.sleep_ms(150)
        u.write(b"AT\r\n")
        time.sleep_ms(450)
        r = u.read()
        r = r.strip() if r else b""
        readable = all(32 <= c < 127 or c in (9, 10, 13) for c in r) if r else None
        flag = ""
        if r and readable:
            flag = "  <== 干净可读！"
        print("  [%6d] %-8s -> %r%s" % (baud, sname, r[:40], flag))
    # 最后再纯发数据试试（不夹 AT）
    u.read()
    u.write(b"\x55" * 40 + b"hello\n")
    time.sleep_ms(400)
    r = u.read()
    r = r.strip() if r else b""
    rd = all(32 <= c < 127 or c in (9, 10, 13) for c in r) if r else None
    print("  [%6d] 0x55+数据 -> %r%s" % (baud, r[:40], "  <== 干净可读！" if (r and rd) else ""))

print()
print("=== 另外：连发同步后长时间收，看有没有主动上报 ===")
u = machine.UART(1, baudrate=9600, tx=17, rx=18, timeout=200, rxbuf=2048)
u.read(); u.write(b"\x55" * 200); time.sleep_ms(200); u.read()
t0 = time.ticks_ms(); acc = b""
while time.ticks_diff(time.ticks_ms(), t0) < 3000:
    r = u.read()
    if r: acc += r
    time.sleep_ms(20)
print("  同步后静止 3 秒收到: %r" % acc[:60])
print("=== 完 ===")

# -*- coding: utf-8 -*-
"""A28 命令集扫描：找出模块认得的 AT 指令 + 退出配置模式的指令。
判据：任何非空应答都打印出来（尤其 'OK'）。"""
import machine, time

CMDS = [
    b"AT", b"AT+VER", b"AT+VERSION", b"AT+HELP", b"AT+CFG", b"AT+CONFIG",
    b"AT+PARAM", b"AT+READ", b"AT+STATUS", b"AT+MODE", b"AT+EXIT", b"AT+QUIT",
    b"AT+RESET", b"AT+RST", b"AT+DEFAULT", b"AT+FACTORY", b"AT+BAUD", b"AT+CHANNEL",
    b"AT+CH", b"AT+POWER", b"AT+ADDR", b"AT+NETID", b"AT+ID", b"AT+SAVE", b"AT+WRITE",
    b"AT+RSSI", b"AT+TEST", b"AT+ENTM", b"AT+CWMODE", b"AT+UART", b"AT+ROLE",
    b"AT+MASTER", b"AT+SLAVE", b"AT+PAIR", b"AT+BIND", b"AT+WORKMODE",
]

u = machine.UART(1, baudrate=9600, tx=17, rx=18, timeout=250, rxbuf=2048)
time.sleep_ms(200)
u.read()
print("=== 扫描 %d 条指令（9600 8N1, 结尾 \\r\\n）===" % len(CMDS))

hits = []
for c in CMDS:
    u.read()
    u.write(c + b"\r\n")
    time.sleep_ms(300)
    r = u.read()
    r = r.strip() if r else b""
    tag = ""
    if r:
        tag = "  <== 有应答"
        hits.append((c.decode(), r[:60]))
    print("%-20s -> %r%s" % (c.decode(), r[:60], tag))

print("\n=== 汇总：%d 条有应答 ===" % len(hits))
for k, v in hits:
    print("  %s -> %r" % (k, v))

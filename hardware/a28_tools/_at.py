# -*- coding: utf-8 -*-
"""A28 模块 AT 探测：直接从 ESP32 的 UART1(GPIO17/18) 问模块话。
目的：判断模块是不是卡在"配置模式"（配置模式 = 9600 8N1，只认 AT 指令，不发数据）。"""
import machine, time

print("=== A28 AT 探测开始 ===")

for baud in (9600, 115200, 38400, 19200, 4800):
    u = None
    try:
        u = machine.UART(1, baudrate=baud, tx=17, rx=18, timeout=200, rxbuf=1024)
        time.sleep_ms(150)
        u.read()                       # 清空
        time.sleep_ms(500)
        # 先被动听 1.2 秒：不主动发东西，看模块自己说不说话
        pas = u.read()
        if pas:
            print("[%d] 被动收到: %r" % (baud, pas[:80]))
        for cmd in (b"AT\r\n", b"AT+VER\r\n", b"AT+VERSION\r\n", b"AT+HELP\r\n"):
            u.write(cmd)
            time.sleep_ms(350)
            r = u.read()
            print("[%d] %-14s -> %r" % (baud, cmd.decode().strip(), r[:80] if r else b""))
    except Exception as e:
        print("[%d] 异常 %s" % (baud, e))

print("=== 探测结束 ===")

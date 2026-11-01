# -*- coding: utf-8 -*-
"""A28 静止判别测试：
关键问题——ERROR 是"对我们数据的应答"还是"模块自己的周期自检报错"？
方法：先完全不发任何字节静止 12 秒，看模块说不动话。"""
import machine, time

def drain(u, tag, secs):
    t0 = time.ticks_ms()
    got = b""
    while time.ticks_diff(time.ticks_ms(), t0) < secs * 1000:
        r = u.read()
        if r:
            got += r
        time.sleep_ms(30)
    n = got.count(b"ERROR")
    print("  [%s] %d 秒内共 %d 字节，含 ERROR x%d : %r" % (tag, secs, len(got), n, got[:70]))
    return n

print("=== A28 静止判别 ===")

u = machine.UART(1, baudrate=9600, tx=17, rx=18, timeout=200, rxbuf=2048)
time.sleep_ms(300); u.read()

# ---- 阶段 1：完全不说话，静止 12 秒 ----
print("阶段1：本机完全不发送，静止 12 秒")
n_idle = drain(u, "静止", 12)

# ---- 阶段 2：只发 1 个字节 ----
print("阶段2：只发 1 个字节 'A'")
u.read(); u.write(b"A"); time.sleep_ms(100)
n_one = drain(u, "1字节", 4)

# ---- 阶段 3：发一行普通文本 ----
print("阶段3：发一行 'hello\\n'")
u.read(); u.write(b"hello\n"); time.sleep_ms(100)
n_line = drain(u, "1行", 4)

# ---- 阶段 4：发一行真遥测 JSON ----
print("阶段4：发一行遥测 JSON")
u.read()
u.write(b'{"p":1.0,"r":2.0,"y":3.0}\n'); time.sleep_ms(100)
n_json = drain(u, "JSON", 4)

# ---- 阶段 5：连续快发 20 行，看 ERROR 是否跟着涨 20 ----
print("阶段5：连发 20 行，看 ERROR 是否同步涨 20")
u.read()
for i in range(20):
    u.write(b"line%d\n" % i)
    time.sleep_ms(50)
n_burst = drain(u, "20行", 5)

print()
print("=== 结论判据 ===")
print("静止=%d  1字节=%d  1行=%d  JSON=%d  20行=%d" % (n_idle, n_one, n_line, n_json, n_burst))
if n_idle >= 5:
    print(">>> 静止时就一直报 ERROR：这是模块【自己的周期自检/链路故障报告】")
    print(">>> 与我们的数据无关 => 模块认为无线链路建不起来（配对/信道/天线/对端）")
elif n_burst > n_line * 3:
    print(">>> ERROR 随发送量同步增长：这是【对我们数据的拒绝应答】=> 模式问题")
else:
    print(">>> 结果不典型，见上面明细")

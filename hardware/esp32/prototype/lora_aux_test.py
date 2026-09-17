"""
ESP32-S3 LoRa 模块健康检测 (AUX 引脚法)
- LoRa UART1: TX=GPIO17, RX=GPIO18
- LoRa AUX: 接到 GPIO4 (可用空引脚)
- 原理: 模块处理数据时 AUX 会变高/变低
- 如果模块 MCU 活着, AUX 会跳动; 如果模块死了, AUX 一直不变
"""
import machine
import time

uart = machine.UART(1, baudrate=9600, tx=17, rx=18)
aux = machine.Pin(4, machine.Pin.IN, machine.Pin.PULL_UP)

print("=== LoRa 模块 AUX 健康检测 ===")
print("确认: LoRa AUX 脚 -> GPIO4")
print("       LoRa UART TXD -> GPIO18, RXD -> GPIO17")
print("       模块 5V 供电")
print()

# 先读初始 AUX 状态
time.sleep(1)
initial = aux.value()
print("初始 AUX 电平: %d (1=空闲高, 0=忙低)" % initial)
print()

# 发送数据并监测 AUX 变化
changes = 0
test_msg = b'HELLO-AUX-TEST-12345'
for i in range(10):
    # 发送前记录
    before = aux.value()
    uart.write(test_msg)
    # 监测 AUX 500ms
    toggled = False
    last = before
    deadline = time.time() + 0.5
    while time.time() < deadline:
        v = aux.value()
        if v != last:
            toggled = True
            last = v
        time.sleep(0.001)
    
    if toggled:
        changes += 1
        print("[%d] AUX 有变化! 模块活着" % i)
    else:
        print("[%d] AUX 无变化" % i)
    time.sleep(0.5)

print()
if changes > 0:
    print("*** 结论: 模块 MCU 活着 (AUX 正常) ***")
    print("    如果 RF 不通, 可能是天线/配置问题, 模块没死透")
else:
    print("*** 结论: 模块无响应, 可能已损坏 ***")

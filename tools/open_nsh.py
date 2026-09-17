import serial
import time
import sys
import threading

PORT = 'COM9'
BAUD = 1000000

print(f"==========================================")
print(f"  SF32LB52 控制台启动工具 (Team 465)")
print(f"  正在连接 {PORT} @ {BAUD} ...")
print(f"==========================================")

try:
    ser = serial.Serial(
        port=PORT,
        baudrate=BAUD,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.01,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False
    )
except Exception as e:
    print(f"[X] 无法打开串口 {PORT}: {e}")
    print("请确认 PuTTY 和其他终端软件已经完全关闭！")
    input("按回车键退出...")
    sys.exit(1)

running = True

def read_loop():
    while running:
        try:
            if ser.in_waiting > 0:
                data = ser.read(ser.in_waiting)
                try:
                    text = data.decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                except Exception:
                    pass
        except Exception:
            break
        time.sleep(0.001)

t = threading.Thread(target=read_loop, daemon=True)
t.start()

print("\n[*] 正在触发板载硬件复位时序 (先开启接收线程再释放复位)...")
ser.rts = True
ser.dtr = True
time.sleep(0.3)
ser.rts = False
ser.dtr = False
time.sleep(0.1)

print("[*] 正在监听输出 (输入命令后按回车，Ctrl+C 发送到板子，输入 quit 退出脚本):\n")

try:
    while True:
        try:
            cmd = input()
            if cmd.strip().lower() == 'quit':
                break
            ser.write((cmd + '\r\n').encode('utf-8'))
        except KeyboardInterrupt:
            # Ctrl+C 发送到板子（退出 NuttX 程序），不退出脚本
            ser.write(b'\x03')
            print("\n[Ctrl+C -> 板子]")
except EOFError:
    pass
finally:
    running = False
    ser.close()
    print("串口已关闭。")

---
name: esp32-sf32-telemetry
description: ESP32(MicroPython) + 思澈 SF32LB52(openvela/NuttX) 双端遥测系统的烧录、调试与排错流程。当任务涉及 ESP32 MicroPython 上传/离线自启动、思澈 openvela 板级驱动与引脚复用、sftool 烧录、LoRa 半双工双向通信、AMOLED framebuffer 显示、中文点阵字库、按键状态机、GPS/气压/姿态传感器采集时使用。包含本项目踩过的 11 类真实工程问题的检查清单。
---

# ESP32 + 思澈 SF32LB52 双端遥测开发

本项目为"箭载 ESP32-S3 + 地面端 SF32LB52"遥测系统。以下流程与检查项均来自真实调试记录，
按顺序执行可避免重复踩坑。

## 1. 端口与环境约定

| 设备 | 端口 | 参数 |
|---|---|---|
| ESP32-S3 | COM7 | 115200，MicroPython REPL |
| 思澈 SF32LB52 | COM9 | 1000000，NSH 控制台（USB CDC） |

**思澈串口打开后必须显式拉低 RTS/DTR**，否则 SoC 会被压在复位状态，表现为 LCD 不亮、`ERR fb0`：

```python
s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False); s.setDTR(False)      # 关键，漏了这一行屏幕就是黑的
```

## 2. 编译与烧录

```bash
# 编译（在 WSL 的 openvela 目录）
export PATH=/home/dev/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/sbin:/bin
cd /home/dev/openvela
cp <workspace>/ground_station.c apps/examples/uart2hwtest/uart2hwtest.c
cp <workspace>/cn_font.h        apps/examples/uart2hwtest/cn_font.h
cmake --build cmake_out/sf32lb52_devkit_lcd
cp cmake_out/sf32lb52_devkit_lcd/nuttx.bin <workspace>/nuttx.bin

# 烧录
sftool.exe --chip SF32LB52 -p COM9 --connect-attempts 40 \
  write_flash ftab.bin@0x12000000 nuttx.bin@0x12010000
```

**烧录前必须停掉占用串口的程序**（网页连接、串口终端、pyserial 脚本），否则报
`Failed to download stub: waiting for shell prompt`。

**烧录失败时的恢复顺序**（由轻到重）：
1. `--connect-attempts 40` 重试
2. RTS 拉高 2~3 秒断电再拉低，然后重试
3. 仍失败 → 请用户**物理拔插 USB**
4. 注意：若失败信息是 `Failed to connect to the chip`，通常是板子已被异常状态卡死，必须物理断电

## 3. 必查清单（11 类真实问题）

### 3.1 屏幕不亮 / `ERR fb0`
- [ ] 串口是否拉低了 RTS/DTR
- [ ] 面板初始化竞态：程序启动时对 `/dev/fb0` 做 30 次 × 500 ms 重试
- [ ] 首次启动偶发 `ERR fb0` 属面板未就绪，重试即可

### 3.2 LCD 显示异常 / 卡在开机画面
- [ ] **TF 卡与 LCD 不能共用硬件 SPI**。ESP32 端 SD 必须用 `machine.SoftSPI`
- [ ] 同一 SPI 总线被重复 `SPI(id, ...)` 初始化会静默改写引脚

### 3.3 按键无事件
- [ ] 检查 `bsp_pinmux.c` 中该引脚的 `HAL_PIN_Set` **是否被注释掉**（本项目 PA34 就是这种情况）
- [ ] 按键位定义与驱动是否一致（本项目 bit0=KEY2/PA11，bit1=KEY1/PA34）
- [ ] 系统控制台 `/dev/buttons` 原驱动可能硬编码 `NUM_BUTTONS=1`，需扩展
- [ ] 不要用"双键同时按下"做复位：悬空脚会误判成双键，把单键事件吞掉

### 3.4 收到的字段缺失 / 数值恒为 0
- [ ] **行缓冲长度**：地面端 `lora_line` 必须能装下最长包（本项目加到 256 字节）
- [ ] 溢出时若只是 `lora_used = 0` 而不丢弃整行，会把半行当整行解析
- [ ] 检查是哪种 NMEA 语句：**GGA 带定位状态与卫星数，RMC 才带时间；坐标应从 GGA 也解析**
- [ ] `printf` 参数个数必须与格式符完全一致，错位会让后续字段全部变成垃圾值

### 3.5 板子运行一会儿就卡死（输出连续 0x00）
- [ ] **不要在后台程序里 `read(stdin)`**：会和 NSH 争抢控制台导致死锁
- [ ] **不要 `close()/open()` 系统控制台 `/dev/console`**：反复开关会搞崩控制台子系统
- [ ] 串口设备轮换、周期重开等"自愈"逻辑是高风险操作，优先固定单个设备
- [ ] 稳定性验收：连续采集 90 秒，`异常字节数` 必须为 0

### 3.6 按键指令收不到
- [ ] 半双工 LoRa 需要**纯接收窗口**，实测窗口 400 ms 不够，**1 秒才可靠**
- [ ] 指令发送要有非阻塞重复队列（重复 10~20 次，跨 1.6~2 秒）
- [ ] 箭载端要做**命令去重**，否则重复发送会产生重复音效
- [ ] 新阶段音效要能**抢占**旧音效的重发，否则点火音会晚数秒

### 3.7 坐标精度不足
- [ ] 发送端小数位 ≥ 6（5 位只有约 1.1 m）
- [ ] 接收端必须用 `double` 解析，`float` 只有约 7 位有效数字
- [ ] 输出 JSON 也要用 `%.7f`，否则发给网页时被再次截断

### 3.8 引脚复用（SF32LB52）
- [ ] 引脚功能表：`vendor/sifli/chips/sf32lb52/bf0_pin_const.c`
- [ ] 排针未引出的脚（如 PA39/PA40）**没有 UART 功能**，配了也没用
- [ ] PA22/PA23 是 **32K 晶振脚**，短接会让系统卡死
- [ ] 引脚表里 `PAxx_I2C_UART` 是 I2C/UART 共用的模糊复用，配置后需实测验证

### 3.9 SD / TF 卡认不出或挂载失败
- [ ] **波特率优先查**：`SDCard(spi, cs, baudrate)` 第三个参数是"初始化完成后切到的速率"，
      默认 1320000 是给硬件 SPI 的，**SoftSPI 位翻转跑不到那么快**，必须显式降到 400 kHz。
- [ ] **MISO 必须开内部上拉**：多数 TF 模块的 DO 没有上拉，悬空时 ESP32 读到随机电平，
      表现为卡"时好时坏"（实测同一配置出现过连续 10 次全败、也出现过 8 次全过）。
- [ ] **别占用板载 RGB 灯脚**（ESP32-S3 DevKitC 的 GPIO48）：WS2812 会把 SPI 数据当颜色吃掉、
      灯乱亮，同时其输入级拖累 MISO 信号。
- [ ] **引脚不确定就用组合扫描定位**：固定 CS/SCK，遍历 MOSI×MISO，看 CMD0 是否返回 `0x01`。
      比对着丝印猜快得多（本项目代码里 4 个脚错了 3 个，MOSI/MISO 还是接反的）。
- [ ] **上游 `sdcard.py` 两个已知缺陷**：
      ① 读 CSD 的 `CMD9` 没带 `release=False` → 读完 R1 就拉高 CS，卡中止 CSD 数据块，
         报 `timeout waiting for response`；
      ② v2 分支一律 `cdv=1`（块寻址），而 **SDSC 卡（OCR 的 CCS=0）必须字节寻址 `cdv=512`**，
         否则只有 0 号块能读、挂载报 `read error`。应按 CCS 位决定。
- [ ] 报 `no SD card` = CMD0 无响应（查接线/供电）；报 ACMD41 不就绪 = 卡内部初始化失败
      （供电余量/接触），断电重启卡通常能恢复。
- [ ] **写入开销很大**：SoftSPI + 老卡上单次 open→write→close 约 120~250 ms（FAT 开销为主），
      5 Hz 记录会把主循环拖垮 → 攒批写入（如 10 行/次）并把记录周期放宽。
- [ ] 注意：**检查驱动的忙等有没有上限**：上游 `sdcard.py` 的 `write()`/`write_token()` 用
      `while spi.read(1,0xFF)[0]==0: pass` 等卡结束编程，**没有超时**。
      卡一旦进入坏状态就永远出不来，表现为**整个系统卡死**（串口无输出、LoRa 全停、屏幕不刷新），
      用 Ctrl-C 能抓到栈停在 `log_row → sd_flush → sdcard.write`。务必给它加上限并抛错。
- [ ] **写卡必须让位给通信**：接收窗口内不写卡（写一次阻塞几百毫秒，9600 波特下 500ms 就是
      480 字节，远超 128 字节 FIFO，窗口期间的上行命令会被整条丢掉）；
      并且做**自适应节流**——单次落盘超过阈值就把采样率逐级下调，卡恢复后再调回。
      只做攒批还不够：攒批的触发条件写成"滞留 1 秒就落盘"，实测会让每批只剩 1 行，等于没攒。

## 4. 配置修改的正确方式

`.config` 手工追加不会触发 `config.h` 重新生成，必须重新 configure：

```bash
cmake -B cmake_out/sf32lb52_devkit_lcd -S nuttx -GNinja \
  -DBOARD_CONFIG=/home/dev/openvela/vendor_sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh \
  -DEXTRA_FLAGS='-Wno-cpp -Wno-deprecated-declarations'
```

**改配置前先备份板级 defconfig**，本项目曾因配置写入异常把 defconfig 截断成 2 行，
导致整个构建损坏（靠 `cmake_out/**/defconfig` 备份才恢复）。

## 5. 开机自启动

板级 `src/etc/init.d/rcS` 内容（延迟启动，等 LCD 就绪）：

```
sleep 12
uart2hwtest &
```

验证：`ps` 应能看到 `uart2hwtest` 进程。

## 6. 验收脚本模板

```python
import serial, time, re
s = serial.Serial('COM9', 1000000, timeout=0.4)
s.setRTS(False); s.setDTR(False)
time.sleep(2); s.reset_input_buffer()
t0 = time.time(); buf = b''
while time.time() - t0 < 90:                 # 至少 90 秒
    d = s.read(8192)
    if d: buf += d
t = buf.decode('utf-8', 'replace')
zero = sum(1 for b in buf if b == 0)
assert zero == 0, '出现异常字节，板子可能卡死'
assert '"t":' in t, '没有遥测输出'
print('遥测行数', t.count('"t":'), '异常字节', zero)
```

## 7. 排错原则

**先取证，再改代码。** 每次改动都保留可复现的串口日志；
不确定时先加计数器/回执（如 `gs_rx`、`CMD RX`、`ACK`），
用数据区分"没收到"还是"收到了没解析"。

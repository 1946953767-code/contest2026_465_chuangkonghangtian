---
name: esp32-s3-p4x-telemetry
description: 箭载 ESP32-S3(MicroPython) + 地面 ESP32-P4X(openvela/NuttX) 双端遥测系统的开发、烧录、联调与排错。任务涉及 MicroPython 上传/离线自启、P4X 应用编译烧录、LoRa 链路、GNSS(GP21)、姿态解算、OLED/面板显示、网页与 AI 助手时使用。内含本项目真实踩过的坑与验证方法。
---

# ESP32-S3 + ESP32-P4X 双端遥测（当前方案）

箭载：**ESP32-S3 单芯片**（MicroPython），采集 IMU/BMP280/AHT20/GNSS/TF/LoRa/RGB/蜂鸣器。
地面：**ESP32-P4X + openvela(NuttX)**，1024×600 MIPI-DSI 触摸屏，应用 `p4x_gs`。
链路：LoRa A28，9600 8N1，短键 JSON，full 帧 175 B @2 Hz、quick 帧 26 B @4–10 Hz。

> 上一代地面站是思澈 SF32LB52（390×450 AMOLED），代码保留在 `app/chuangkong_ground_station/` 作演进记录，**不再参与构建**。

## 1. 端口与环境

| 设备 | 串口 | 参数 |
| :-- | :-- | :-- |
| ESP32-S3（箭载） | CH340K，如 COM13 | **115200**，MicroPython REPL |
| ESP32-P4X（地面） | USB CDC 303A:1001，如 COM3 | **115200**，NSH 控制台 |

- 打开 P4 串口后**显式 `setDTR(False); setRTS(False)`**，否则会被压在复位状态、屏幕不亮；
- 上传/烧录前必须**释放串口**：浏览器 WebSerial、串口终端、pyserial 脚本都要断开，否则 esptool 报 "port is busy"。

## 2. 箭载端（MicroPython）

```powershell
C:\py312\python.exe -m mpremote connect COM13 cp src\central_tx_s3.py :central_tx.py
C:\py312\python.exe -m mpremote connect COM13 soft-reset
```

离线自启：`main.py` 在开机时 `import central_tx`。

**必查**：

- [ ] `.py` 不能带 **UTF-8 BOM**，否则 MicroPython 报 `invalid syntax`（检查前 3 字节是否 `EF BB BF`）；
- [ ] PSRAM 初始化失败 `Bailing out` 是这块 N8R8 的正常现象，不影响运行；
- [ ] 固件里所有 `print` 都会进 USB 串口，若网页靠串口解析 JSON，注意格式与换行。

## 3. 地面端（openvela / P4X）

```bash
export PATH=/home/dev/tools/riscv-none-elf-gcc/bin:$PATH
cd /home/dev/p4 && cmake --build cmake_out/p4x -j12
cp cmake_out/p4x/nuttx.bin <workspace>/firmware/
```

烧录（BOOT 必须按住，脚本会重试）：

```powershell
python tools\flash.py firmware\openvela_p4x_xxx.bin COM3 900
```

- 烧完若 `UI task did not answer`，用 `esptool --before no-reset --after watchdog-reset read-mac` 复位一次；
- `ps` 里应看到 `tchsamp / p4x_gs / (lora_rx / panel 视构建而定)`。

## 4. 十个真实坑（按踩坑频率排序）

### 4.1 GPS 收不到任何字节
- [ ] **模块 TXD 接的是 ESP32 的 RX**：本项目模块 TXD→**IO10**，固件曾把它写成 TX=IO10 导致完全无数据；
- [ ] 波特率：GP21 多模模组是 **115200**，不是 9600；
- [ ] 语句前缀是 `$GN`（多系统融合），不是 `$GP`。

### 4.2 卫星数"看到很多、只连上几个"
- `sat` = GGA 字段 7（**参与定位**），`satv` = GSV 累加（**视野内**），两者天然不同；
- HDOP>5 时垂直误差可达 ±15 m，**GPS 给不了米级高度**，定高必须靠气压；
- 质量码 `q`（GGA 字段 6）：1 单点 / 2 差分 / **6 估算（等于没锁）**。

### 4.3 高度乱飘 / 显示 -17 m
- 不要写 `fused = 气压相对高度 + GPS 绝对海拔`（把相对量加到绝对量上，必飘）；
- 正确做法：气压换算成绝对海拔（标准海平面气压 101325 Pa）做**预测**，GPS 的海拔以**很大的 R**（如 400）做慢纠正；
- 丢星时**保留**卡尔曼状态，不要重置基准，否则每次重锁都跳变。

### 4.4 姿态"左右倾只会转"
- 欧拉角只有 pitch/roll/yaw，**画不出"左右倒"**，要用力向量定向或按约定换轴；
- 本项目约定：**滚转绕 Z（左右倾）、俯仰绕 X（前后倾）、偏航绕 Y（自转）**；
- 加速度计静止读数指向"上"，所以用 `u=(ax,ay,az)` 对齐世界 +Y，再补一次手性修正。

### 4.5 偏航一直涨
- 无磁力计 → 只能积分，残余零偏会累积；
- 加死区（~1.2 °/s）+ 静止冻结即可，静止 10 s 漂移可压到 0.0°；
- **不要**"静止时在线更新零偏"：慢速倾斜会被误判成静止，把真实转动吃掉。

### 4.6 拔掉 USB 后屏幕定格（背光还亮）
- 后台任务若**阻塞写 `/dev/console`**，USB 线一拔没有接收方，写操作永不返回 → UI 冻死；
- 改非阻塞 + 有限重试（如 2 ms×20 次）后丢弃该帧，UI 绝不卡。

### 4.7 P4 的 printf 在控制台看不到
- 由内核创建的任务可能没有 fd 1；`printf` 静默失败；
- 需要输出时**显式 `open("/dev/console", O_WRONLY)`** 再写，并注意 4.6 的非阻塞要求。

### 4.8 网页"连上了但没数据"
- 检查波特率是否与板子一致（本项目曾按 1000000 打开，桥接串口全是乱码）；
- 板子回显的 JSON 常与 NSH 提示符粘在一行（`nsh> {...}`），解析器要**在行内找 `{...}`**，不能只看行首；
- 高分辨率定时器/端口冲突：旧进程占着端口会让服务"假装启动"，先 `netstat -ano | findstr :端口`。

### 4.9 Windows 服务类脚本
- `.bat` 存成 UTF-8 **BOM** 会让 cmd 第一行乱码、整脚本不执行；
- `pythonw` 下没有 stdout/stderr：`http.server`、直接 `sys.stderr.write` 都会让请求被立刻断开（`ERR_EMPTY_RESPONSE`）→ 自己写静态服务并屏蔽日志。

### 4.10 PPT/文档类
- 拉丁字符若设成中文字体族（如 MiSans），在**没装该字体**的机器上会回退成符号乱码（`SoftSPI`→`⊠⌐§ΨΨ`）；
  正文拉丁用 Arial，中文用 MiSans；
- 视频别按原始 295 MB 嵌进 PPT，压到 720p/15 MB 再换进 `ppt/media/`（pptx 就是 zip）。

## 5. 验收动作（每次改完都跑）

```powershell
# 箭载端：看有没有帧、帧率、关键字段
python -c "import serial,re,json,time; ..."   # 见 tools/ 下的探针脚本

# 地面端：任务表 + 回显帧
python tools\p4x.py status
```

- 连续 90 s 串口采样，**异常字节必须为 0**；
- 静止时高度跨度 < 0.5 m、偏航漂移 < 0.1°；
- 拔掉数据线 30 s，板子屏幕必须继续刷新。
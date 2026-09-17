# 创空航天 · 火箭遥测与地面站系统

> 2026 首届 openvela AI 硬件开发者大赛 ｜ 队伍编号 **465** ｜ 队伍名称 **创空航天**

## 一、作品简介

本作品是一套**自研探空火箭的两端遥测系统**：箭载端用 **ESP32-S3**（MicroPython）
采集 GPS、气压温度、六轴姿态、并写入 MicroSD 卡；数据经 **LoRa** 下传到地面端；
地面端用 **思澈 SF32LB52（openvela / NuttX）** 驱动 390×450 AMOLED 屏，
实时显示定位、时间、姿态、速度、高度与六页曲线，并用触摸翻页、按键发令、
蜂鸣器回执，同时把遥测数据推送到浏览器里的网页遥测大屏。

**要解决的问题**：探空火箭在发射到回收的几分钟里，需要一份**不丢包、可回放**的
飞行数据，以及一个**现场就能读懂状态**的地面指挥界面。

**亮点**：

- **全端侧，无云依赖**：采集、解算、显示、记录全部在两端 MCU 上完成，断网照常工作。
- **能"听见"的指令链路**：地面站按键发令（点火 / 灭火 / 结束）时，箭载端蜂鸣器
  用不同音调与次数回执——现场不看屏幕也知道命令到底传到没传到。
- **半双工时隙调度**：LoRa 是半双工，本作品把关键指令安排进接收窗口的固定时隙
  并重复发送 20 次 + ACK 回执，做到关键指令低延迟、不丢包；遥测流则自适应节流，
  保证不丢包。
- **真机跑通**：两端均已完成实机联调，SD 卡实测落盘 1523 行 × 18 列 CSV。

## 二、选题方向

**AI 硬件产品创新**。

理由：本作品是完整的软硬件自研产品（自绘 PCB 规格、板级驱动适配、双端固件、
上位机大屏），核心价值在"硬件 + 端侧算法 + 现场可用性"，
正好落在 AI 硬件产品创新赛道；同时开发全过程由 AI Agent 深度参与（见第五节）。

## 三、目录结构

```text
contest2026_465_chuangkonghangtian/
├── app/chuangkong_ground_station/   地面站应用（思澈 SF32LB52 / openvela）
│   ├── ground_station.c               应用主体：LoRa 收发 / NMEA 解析 / 姿态高度解算 /
│   │                                  六页 AMOLED 界面 / 触摸翻页 / 按键状态机 / 蜂鸣器
│   ├── cn_font.h                      中文点阵字库
│   └── CMakeLists.txt / Makefile / Make.defs / Kconfig   应用打包文件
├── board/contest_board/             板级适配
│   ├── Kconfig / configs / src/       组委会脚手架骨架（保留）
│   └── delta/                       ★ 本作品的板级改动（相对 vendor/sifli 出厂板级）
│       ├── bsp_pinmux.c               引脚复用：LoRa(UART2 PA20/PA27)、修好被注释的 KEY1(PA34)、TF 卡、触摸
│       ├── sf32lb52_buttons.c         /dev/buttons 按键驱动（扩展为双键）
│       ├── sifli_ap.c                 LCD 面板上电与 framebuffer 初始化重试
│       ├── sifli_bitbang_i2c.c        触摸 FT6146 的 bit-bang I2C
│       ├── defconfig                  本作品的 NuttX 配置（实测烧录用）
│       └── rcS                        开机自启动脚本
├── hardware/esp32/                  箭载端 ESP32-S3（MicroPython）
│   ├── main.py                        离线自启动入口
│   ├── central_tx.py                  主程序：多任务采集 / LoRa 收发 / 蜂鸣器 / RGB / SD 记录
│   ├── drv.py                         GPS / BME280 / MPU6050 / TFT 驱动
│   ├── sdcard.py                      修好的 MicroPython SD 卡驱动（见技术报告 3.5）
│   └── prototype/                     早期传感器与显示验证脚本（保留开发过程）
├── web/dashboard.html               网页遥测大屏（曲线 + 雷达图 + 状态面板）
├── skills/esp32-sf32-telemetry/     自建 Skill（AI 开发用，见第五节）
├── tools/                           构建、烧录、取证、日志导出脚本
├── evidence/                        真机证据：SD 卡 CSV、串口日志、实物照片、演示视频
├── docs/                            技术报告、证据说明；ai-coding-raw/ 为 AI 日志原始记录
├── logs/1946953767-code/            AI Coding 对话日志（组委会要求的结构）
└── contest2026_465_chuangkonghangtian.xml   本仓 manifest（app / board 的软链映射）
```

## 四、运行方式

### 4.1 获取工程

```bash
repo init -u https://github.com/open-vela/contest2026_465_chuangkonghangtian \
  -b dev-ai-contest-2026 -m contest2026_465_chuangkonghangtian.xml
repo sync -c -j8
```

同步后本仓位于工作树 `contest2026_465_chuangkonghangtian/`，
`app/` 与 `board/` 会按 manifest 里的 `<linkfile>` 软链到
`packages/demos/` 与 `vendor/openvela/boards/` 对应位置。

### 4.2 编译与烧录地面站（思澈 SF32LB52）

实测环境为 WSL + Sifli SF32LB52 开发板（外接 LCM 适配板）：

```bash
cd <openvela 工作区>
cp contest2026_465_chuangkonghangtian/app/chuangkong_ground_station/ground_station.c \
   apps/examples/uart2hwtest/uart2hwtest.c
cp contest2026_465_chuangkonghangtian/app/chuangkong_ground_station/cn_font.h \
   apps/examples/uart2hwtest/cn_font.h

cmake --build cmake_out/sf32lb52_devkit_lcd
cp cmake_out/sf32lb52_devkit_lcd/nuttx.bin .
```

烧录（`ftab.bin` 为分区表，两者一起写）：

```bash
sftool.exe --chip SF32LB52 -p COM9 --connect-attempts 40 \
  write_flash ftab.bin@0x12000000 nuttx.bin@0x12010000
```

> 注意：串口工具打开 COM9 后必须显式拉低 RTS/DTR，否则 SoC 会被压在复位状态，
> 表现为屏幕不亮、`ERR fb0`。烧录前请先关闭占用串口的程序。

### 4.3 烧录箭载端（ESP32-S3）

```bash
mpremote connect COM7 cp hardware/esp32/main.py       :main.py
mpremote connect COM7 cp hardware/esp32/central_tx.py :central_tx.py
mpremote connect COM7 cp hardware/esp32/drv.py        :drv.py
mpremote connect COM7 cp hardware/esp32/sdcard.py     :sdcard.py
```

`main.py` 为离线自启动入口，重新上电即自动运行，无需电脑。

**接线（已实测验证）**：

| 功能 | 引脚 |
|---|---|
| LoRa（UART1） | TX=17 RX=18 |
| GPS（UART2） | TX=39 RX=38 |
| TF 卡（SoftSPI） | CS=14 SCK=21 MOSI=47 MISO=4 |
| TFT（SPI2） | SCK=12 MOSI=11 CS=7 DC=46 RST=10 BL=5 |
| 蜂鸣器（无源，PWM） | BUZZ=35 |
| MPU6050（SoftI2C） | SCL=9 SDA=3 |
| BME280 / BMP280（SoftI2C） | SCL=1 SDA=2 |
| 板载 RGB（NeoPixel） | 48 |

> TF 卡必须使用 `machine.SoftSPI`：TFT 已占用硬件 SPI2，共用会使 LCD 花屏或卡死在开机画面。
> 另外 GPIO48 是板载 WS2812 的数据脚，MISO 不要接到 48——会污染 SPI 数据线。

### 4.4 网页遥测大屏

直接双击 `web/dashboard.html` 用浏览器打开，按页面提示选择串口设备即可。
曲线与雷达图在浏览器本地渲染，不依赖外网。

### 4.5 操作流程（现场）

| 操作 | 效果 |
|---|---|
| KEY2（待命） | 点火确认：低音长鸣一声（440 Hz） |
| KEY1（确认后） | 点火：5 声尖锐短鸣（3200 Hz），新建 SD 记录文件，RGB 点亮 |
| KEY2（飞行中） | 灭火：2 声低沉（300 Hz），RGB 点亮 |
| KEY1（灭火后） | 任务结束：4 声递降，RGB 熄灭，新建记录文件 |

## 五、AI Coding 使用说明

本作品的全部代码、板级驱动适配、真机调试与文档，均由 **DSH（DeepSeek Harness）AI Agent**
在对话中协作完成。开发方式是"人定需求与验收、AI 读代码改代码编译烧录跑真机"的闭环：

| 环节 | AI 的参与方式 |
|---|---|
| 需求拆解 | 把"点火要响、命令要低延迟不丢包"这类现场口述需求，拆成可验证的固件行为 |
| 方案设计 | 半双工时隙调度、自适应落盘节流、加速度积分 + GPS 融合解算高度等方案均由 AI 给出并逐轮迭代 |
| 编码 | 两端固件、板级驱动、网页大屏、字体生成与日志工具约 7180 行有效代码 |
| 调试 | 由 AI 直接操作串口与烧录工具读真机日志、看栈回溯、定位硬件故障（典型 11 类问题见技术报告 3.5） |
| 经验固化 | AI 把踩过的坑自动沉淀成自建 Skill `skills/esp32-sf32-telemetry/`，后续同类任务直接命中 |
| 文档 | 技术报告、证据整理、本 README 均由 AI 依据原始记录生成 |

**规模**（详见 `docs/技术报告.docx` 3.6 节）：收录 11 个开发会话，
7143 次模型调用、6489 次工具调用、1404 条用户提问，累计消耗约 9124 万 token；
约 95% 的有效代码由 AI 生成并经真机验证。

**关于日志工具与本仓 `logs/` 的说明（如实声明）**

本届组委会的日志采集器支持 Claude Code / AIoT-IDE / OpenCode / Codex 四种工具，
并要求在被采集机器上安装 hook、在 openvela 工作区内工作。本队实际使用的是
**DSH（DeepSeek Harness）**，不在上述工具列表内，因此**没有采集器产出的原始日志**。

为保证评审可核查，我们做了两件事：

1. `logs/1946953767-code/` 下按组委会要求的结构提交日志
   （`manifest.json` + `<日期>/<工具名>__<会话id>.jsonl`，`seq` 为会话内递增序号、无断档）。
   内容由 DSH 会话记录**逐条转换**而来，字段对齐组委会《AI Coding 日志归集与提交手册》
   第四节（`text` / `thinking` / `tool_name,input,output` / `model` / `seq`）。
   共 11 个会话、19194 条事件；**对话正文与工具调用原样搬运，未做改写或摘要**，
   仅过滤了系统自动注入的运行环境提示（`Current runtime context` / `system-reminder`
   等并非人说的话的块）——过滤掉的条数已逐会话记录在
   `tools/ai_logs/export_official_logs.js` 的运行输出中。
   转换脚本本身也在仓库内：`tools/ai_logs/export_official_logs.js`，可重跑复核。
2. DSH 导出的原始记录（未经转换的 `.jsonl`、可读转录、会话索引与收录/排除理由）
   一并保留在 `docs/ai-coding-raw/`，可与 `logs/` 内容逐条对照。

我们理解这不等同于官方采集器的产物，评委如需核验，两处记录可以互相印证。

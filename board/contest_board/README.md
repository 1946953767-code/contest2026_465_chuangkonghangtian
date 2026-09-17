# 板级适配（思澈 SF32LB52 DevKit + LCM 适配板）

`repo sync` 后本目录会软链到工作树的 `vendor/openvela/boards/contest2026_465_board`。

## 目录说明

组委会脚手架在这里给的是**一套通用板级骨架**（`Kconfig` / `configs/nsh/defconfig` / `src/`），
本作品保留不动；我们真正的板级改动全部放在 **`delta/`** 里，一眼能看出"相对出厂板级改了什么"。

```text
board/contest_board/
├── Kconfig                     组委会脚手架（保留）
├── configs/nsh/defconfig       组委会脚手架（保留）
├── src/                        组委会脚手架骨架 + board_boot.c（保留）
├── README.md                   本文件
└── delta/                      ★ 本作品的板级改动（相对 vendor/sifli 出厂板级）
    ├── CMakeLists.txt          出厂板级的 src/CMakeLists.txt（供对照，说明各源文件在 vendor 树中的位置）
    ├── board.h                 板级头文件
    ├── bsp_pinmux.c            引脚复用（改动）
    ├── sf32lb52_buttons.c      /dev/buttons 按键驱动（改动）
    ├── sifli_ap.c              LCD 上电与 framebuffer 初始化（改动）
    ├── sifli_bitbang_i2c.c     触摸 FT6146 的 bit-bang I2C（改动）
    ├── defconfig               本作品实测烧录用的 NuttX 配置
    └── rcS                     开机自启动脚本
```

> `delta/` 里的文件在实测工程中的原位置是
> `vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/`（`rcS` 在 `src/etc/init.d/`）。
> 它们**不是**一套从零写的板级移植，而是本作品在出厂板级上做的改动增量，
> 因此按"增量"组织、不参与脚手架骨架的构建，避免与组委会骨架冲突。

## 逐项改动与原因

| 文件 | 相对出厂板级改了什么 | 为什么 |
|---|---|---|
| `bsp_pinmux.c` | 放开 LoRa 用的 UART2（PA20/PA27）引脚复用；**修好被注释掉的 KEY1（PA34）**；确认 TF 卡（PA24/25/28/29）与触摸（PA30/31/33）复用 | KEY1 的 `HAL_PIN_Set` 在原文件里被注释掉，这是"按键无事件"的根因 |
| `sf32lb52_buttons.c` | 原驱动硬编码 `NUM_BUTTONS=1`，扩展为双键：`/dev/buttons` 的 bit0=KEY2(PA11)、bit1=KEY1(PA34) | 本作品要 KEY1/KEY2 两个键做任务阶段控制；另外**不能用"双键同按"做复位**——悬空脚会误判成双键，把单键事件吞掉 |
| `sifli_ap.c` | LCD 面板上电与 `/dev/fb0` 初始化加重试（30 次 × 500 ms） | 冷启动偶发 `ERR fb0`（面板未就绪），不重试就黑屏 |
| `sifli_bitbang_i2c.c` | 触摸 FT6146 改走 bit-bang I2C | 硬件 I2C 读回在 0xFF / 0x00 之间跳变，触摸不稳定 |
| `defconfig` | 打开 UART2 硬件测试应用、framebuffer、触摸、按键 | 本作品的实测配置 |
| `rcS` | 开机自动启动 `uart2hwtest`（地面站应用） | 现场上电即用，不需要接电脑敲命令 |

## 两条最容易踩的坑（已固化进 Skill）

1. **串口工具打开 COM9 后必须显式拉低 RTS/DTR**，否则 SoC 被压在复位状态，表现为屏幕不亮、`ERR fb0`。
2. **TF 卡与 LCD 不能共用硬件 SPI**：同一 SPI 总线被重复 `SPI(id, ...)` 初始化会静默改写引脚；
   箭载端 ESP32 的 SD 卡因此改用 `machine.SoftSPI`。

以上排查项已沉淀进 `skills/esp32-sf32-telemetry/SKILL.md`（11 类真实工程问题检查清单）。

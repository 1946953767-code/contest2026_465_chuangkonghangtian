# 地面站应用（思澈 SF32LB52 / openvela）

火箭遥测系统的**地面端**主程序：单文件 C 应用，直接跑在 SF32LB52 上，
负责 LoRa 收发、NMEA 解析、姿态与高度解算、AMOLED 六页界面、触摸翻页、
按键状态机与蜂鸣器提示音。

| 文件 | 说明 |
|---|---|
| `ground_station.c` | 应用主体（2255 行，约 2200 行有效代码） |
| `cn_font.h` | 中文点阵字库（16×16，由 `tools/gen_cn_font.py` 生成） |
| `CMakeLists.txt` / `Makefile` / `Make.defs` / `Kconfig` | openvela 应用打包文件 |

## 应用名与 Kconfig 符号

应用名与配置符号**保持组委会脚手架之外实测工程的原值不变**，
以便 `board/contest_board/defconfig` 与真机烧录的固件配置完全一致：

- 可执行名：`uart2hwtest`
- Kconfig 符号：`CONFIG_EXAMPLES_UART2HWTEST`
- 参与编译的源文件：`ground_station.c`

实测工程里本文件位于 `apps/examples/uart2hwtest/`，文件名 `uart2hwtest.c`，
内容与仓库内 `ground_station.c` 相同（仓库内按作品可读性重命名）。

## 编译进 openvela

仓库根目录 `contest2026_465_chuangkonghangtian.xml` 里已配好软链：

```xml
<linkfile src="app/chuangkong_ground_station"
          dest="packages/demos/contest2026_465_chuangkong_ground_station"/>
```

`repo sync` 之后，本目录会被软链到工作树的
`packages/demos/contest2026_465_chuangkong_ground_station`，
在 menuconfig 中勾选 `EXAMPLES_UART2HWTEST` 即可参与编译。

实测（Sifli SF32LB52 开发板 + LCM 适配板）的完整编译烧录命令见仓库根目录
`README.md` 第四节，以及 `skills/esp32-sf32-telemetry/SKILL.md`。

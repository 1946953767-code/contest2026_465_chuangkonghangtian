# 箭载端固件（ESP32-S3 / MicroPython）

| 文件 | 说明 |
|---|---|
| `main.py` | 离线自启动入口。上电即运行，无需电脑 |
| `central_tx.py` | 主程序（628 行）：GPS/气压/姿态多任务采集、LoRa 双向收发、蜂鸣器阶段音效、RGB 阶段指示、SD 卡 CSV 记录（自适应节流 + 断线重连） |
| `drv.py` | GPS(NMEA)、BME280/BMP280、MPU6050、TFT(ST7735) 驱动 |
| `sdcard.py` | MicroPython SD 卡驱动（本项目修好的版本，见下方"三处修复"） |
| `buzzer_drv.py` | 无源蜂鸣器 PWM 驱动（GPIO35） |
| `mpu6050.py` | 六轴姿态读取 |
| `sd_health.py` / `sd_test.py` / `sd_blk.py` | SD 卡健康检查与读写验证脚本（现场排查用） |
| `prototype/` | 早期各传感器与显示的分项验证脚本，保留开发过程 |

## 烧录

```bash
mpremote connect COM7 cp main.py       :main.py
mpremote connect COM7 cp central_tx.py :central_tx.py
mpremote connect COM7 cp drv.py        :drv.py
mpremote connect COM7 cp sdcard.py     :sdcard.py
mpremote connect COM7 cp buzzer_drv.py :buzzer_drv.py
mpremote connect COM7 cp mpu6050.py    :mpu6050.py
```

## `sdcard.py` 的三处修复（原上游驱动的真实缺陷）

1. **写忙等没有超时**：原实现 `while self.spi.read(1, 0xFF)[0] == 0: pass` 会永久卡死，
   表现为整机无串口、LoRa 静默、屏幕冻结。已加 0.6 s / 0.8 s 写超时上限，超时抛 `OSError`。
2. **CMD9 提前释放 CS**：上游实现使 CSD 块读到一半被中止，报 `timeout waiting for response`。
   改为 `self.cmd(9, 0, 0, 0, False)`（`release=False`），出错时再补 `self.cs(1)`。
3. **寻址方式硬编码**：v2 分支无条件 `cdv = 1`（块寻址），但 SDSC 卡需要字节寻址，
   报 `read error`。已改为读 OCR 寄存器动态判定：`self.cdv = 1 if (ocr[0] & 0x40) else 512`。

## 接线（已实测验证）

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

> 注意：GPIO48 是板载 WS2812 的数据脚，MISO 不要接 48——会污染 SPI 数据线导致 SD 卡识别失败。

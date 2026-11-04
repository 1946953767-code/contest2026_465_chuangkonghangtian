# p4x_gs · ESP32-P4X 地面站应用（当前方案）

这是**实际在跑**的地面站应用：ESP32-P4X + openvela/NuttX，1024×600 MIPI-DSI 触摸屏。

## 怎么放进 openvela 树

```bash
cp -r app/p4x_gs  <openvela>/apps/examples/p4x_gs
```

然后在板级 defconfig 里打开：

```
CONFIG_EXAMPLES_P4X_GS=y
CONFIG_EXAMPLES_P4X_GS_PROGNAME="p4x_gs"
CONFIG_EXAMPLES_P4X_GS_PRIORITY=100
CONFIG_EXAMPLES_P4X_GS_STACKSIZE=8128
```

编译（RISC-V 工具链需在 PATH）：

```bash
cmake -B build -S <openvela>/ov-cmake \
  -DBOARD_CONFIG=<board>/configs/nsh \
  -DCMAKE_TOOLCHAIN_FILE=<openvela>/ov-cmake/arch/risc-v/src/cmake/Toolchain.cmake \
  -DNUTTX_APPS_DIR=<openvela>/apps -GNinja
cmake --build build -j12
```

## 这个应用做了什么

| 模块 | 说明 |
| :-- | :-- |
| LoRa 收包 | 独立任务（prio 110），按 `{` … `}` 严格帧同步，只喂完整帧给解析器 |
| 遥测解析 | 短键 JSON（`p/r/y/a/s/ph/la/lo/...`），带野值门限与平滑 |
| 姿态渲染 | 3D 火箭模型，重力向量定向；滚转绕 Z、俯仰绕 X、偏航绕 Y |
| 页面 | 状态 / 曲线 / 速度 / 火箭姿态 / 雷达，触摸翻页 |
| 阶段闭环 | 按住 2 秒发令、5 秒冷却、等 `ph` 回报解锁 |
| 网页回显 | 每 2 帧把遥测按长键 JSON 打到 `/dev/console`（非阻塞，拔线不会卡死 UI） |

# 465 创空航天 — 地面站应用（思澈 SF32LB52）
#
# 源码文件名沿用实测工程里的 ground_station.c；
# 应用名与 Kconfig 符号保持 uart2hwtest / CONFIG_EXAMPLES_UART2HWTEST 不变，
# 这样 board/contest_board/defconfig 与实测烧录的固件配置完全一致。
if(CONFIG_EXAMPLES_UART2HWTEST)
  nuttx_add_application(NAME uart2hwtest SRCS ground_station.c STACKSIZE 4096 PRIORITY 100)
endif()

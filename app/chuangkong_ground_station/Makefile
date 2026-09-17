############################################################################
# 465 创空航天 — 地面站应用 Makefile（OpenVELA / NuttX apps 规范）
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = uart2hwtest
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_EXAMPLES_UART2HWTEST)

MAINSRC = ground_station.c

include $(APPDIR)/Application.mk

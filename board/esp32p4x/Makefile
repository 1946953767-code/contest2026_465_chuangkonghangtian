############################################################################
# vendor/espressif/boards/esp32p4x-function-ev-board/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = board_boot.c board_bringup.c

ifeq ($(CONFIG_DEV_GPIO),y)
ifneq ($(CONFIG_GPIO_LOWER_HALF),y)
CSRCS += board_gpio.c
endif
endif

ifeq ($(CONFIG_ESP32P4X_DISPLAY),y)
CSRCS += board_display.c
endif

ifeq ($(CONFIG_BOARDCTL_RESET),y)
CSRCS += board_reset.c
endif

include $(TOPDIR)/boards/Board.mk

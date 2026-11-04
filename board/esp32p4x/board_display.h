/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_display.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ESPRESSIF_BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_DISPLAY_H
#define __VENDOR_ESPRESSIF_BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_DISPLAY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_display_initialize
 *
 * Description:
 *   Bring up the EK79007 1024x600 MIPI-DSI panel and register /dev/fb0.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int board_display_initialize(void);

/****************************************************************************
 * Name: board_display_uninitialize
 *
 * Description:
 *   Turn the backlight off and stop DSI video.
 *
 ****************************************************************************/

void board_display_uninitialize(void);

#endif /* __VENDOR_ESPRESSIF_BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_DISPLAY_H */

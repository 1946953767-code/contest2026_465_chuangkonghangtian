/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_panel.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_PANEL_H
#define __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_PANEL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_panel_resync
 *
 * Description:
 *   Stop and restart the MIPI DSI video stream and re-assert the backlight,
 *   for recovering a panel that latched a blank frame while the software was
 *   still publishing pictures.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_panel_resync(void);

/****************************************************************************
 * Name: board_panel_recover
 *
 * Description:
 *   Panel recovery: pulse the reset line (GPIO27), restart the DSI video
 *   stream and switch the backlight on.  Used by the UI watchdog when the
 *   panel stops showing the frames that are being published.
 *
 ****************************************************************************/

int board_panel_recover(void);

#endif /* __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_PANEL_H */

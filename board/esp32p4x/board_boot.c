/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_boot.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>

#include "board_pins.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_board_late_init
 *
 * Description:
 *   Optional late board hook.  Nothing required for the USB console
 *   milestone; kept as an explicit, reviewable placeholder.
 *
 ****************************************************************************/

void esp_board_late_init(void)
{
}

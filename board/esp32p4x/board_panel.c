/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_panel.c
 *
 * Panel stream recovery for ESP32-P4X-Function-EV-Board.
 *
 * The panel once showed a blank (lit) screen while the UI kept publishing
 * complete frames, which points at the DSI bridge holding a stale frame or a
 * video stream that stopped re-arming.  Restarting the stream and re-asserting
 * the backlight brings the picture back, so the UI can call this when the
 * panel looks dead but the software is healthy.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include "espressif/esp_gpio.h"
#include "esp32p4_mipi_dsi.h"

#include "board_panel.h"

#ifdef CONFIG_ESP32P4X_MIPI_DSI

#define BOARD_LCD_BACKLIGHT_GPIO 26

/* P4X-PANEL-WD: raw register access for the panel reset line.  esp_configgpio()
 * on this port has taken the board down before, so only latch writes are used.
 */

#define P4X_GPIO_BASE       0x500e0000ul
#define P4X_GPIO_OUT_W1TS   (P4X_GPIO_BASE + 0x08ul)
#define P4X_GPIO_OUT_W1TC   (P4X_GPIO_BASE + 0x0cul)
#define P4X_GPIO_ENABLE_W1TS (P4X_GPIO_BASE + 0x24ul)
#define P4X_PANEL_RST_PIN   27

static void p4x_rst(int level)
{
  if (level)
    {
      *(volatile unsigned int *)P4X_GPIO_OUT_W1TS = 1u << P4X_PANEL_RST_PIN;
    }
  else
    {
      *(volatile unsigned int *)P4X_GPIO_OUT_W1TC = 1u << P4X_PANEL_RST_PIN;
    }
}

int board_panel_recover(void)
{
  /* Full recovery: reset the panel, restart the stream, backlight on. */

  *(volatile unsigned int *)P4X_GPIO_ENABLE_W1TS = 1u << P4X_PANEL_RST_PIN;

  p4x_rst(0);
  usleep(20000);
  p4x_rst(1);
  usleep(150000);

  esp32p4_mipi_dsi_stop_video();
  usleep(30000);

  if (esp32p4_mipi_dsi_start_video() < 0)
    {
      return -EIO;
    }

  esp_gpiowrite(BOARD_LCD_BACKLIGHT_GPIO, true);
  return OK;
}

int board_panel_resync(void)
{
  int ret;

  esp32p4_mipi_dsi_stop_video();
  usleep(30000);

  ret = esp32p4_mipi_dsi_start_video();
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(BOARD_LCD_BACKLIGHT_GPIO, true);
  return OK;
}

#else

int board_panel_resync(void)
{
  return -ENOSYS;
}

#endif /* CONFIG_ESP32P4X_MIPI_DSI */

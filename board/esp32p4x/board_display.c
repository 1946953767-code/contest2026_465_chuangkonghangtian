/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * EK79007 7" MIPI-DSI panel bring-up for the ESP32-P4X Function EV Board.
 *
 * Panel parameters and the power-on sequence were taken from a locally
 * verified bring-up of this exact panel/board combination, then re-expressed
 * on top of the openvela ESP32-P4 DSI driver:
 *
 *   /dev/fb0, 1024x600 RGB565, 2 data lanes @ 1000 Mbps/lane, 48 MHz DPI clock
 *   reset      = GPIO27 (J1 pin 38)
 *   backlight  = GPIO26 (J1 pin 31)
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_ESP32P4X_DISPLAY

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/video/fb.h>

#include <arch/chip/esp32p4_mipi_dsi.h>
#include <arch/chip/gpio_sig_map.h>

#include "hal/gpio_hal.h"

#include "espressif/esp_gpio_p4.h"

#include "board_display.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_LCD_RESET_GPIO       27
#define BOARD_LCD_BACKLIGHT_GPIO   26

#define BOARD_LCD_WIDTH            1024
#define BOARD_LCD_HEIGHT           600
#define BOARD_LCD_BPP              16      /* RGB565 */

#define BOARD_LCD_DCS_SETTLE_MS    120
#define BOARD_LCD_TEST_COLOR       0x001f  /* Blue, so a lit panel is obvious */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct board_panel_cmd_s
{
  uint8_t command;
  uint8_t value;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct board_panel_cmd_s g_ek79007_init[] =
{
  {0xb2, 0x10},   /* Two DSI data lanes */
  {0x80, 0x8b},
  {0x81, 0x78},
  {0x82, 0x84},
  {0x83, 0x88},
  {0x84, 0xa8},
  {0x85, 0xe3},
  {0x86, 0x88}
};

/****************************************************************************
 * Private Functions

 ****************************************************************************/

/****************************************************************************
 * Name: board_display_gpio_initialize
 *
 * Description:
 *   Configure the panel reset and backlight GPIOs and release reset.  The
 *   backlight is left off so the panel never lights before it has a frame.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: p4_busy_delay
 *
 * Description:
 *   Millisecond delay that does not depend on the system tick.  The panel is
 *   powered up from board_app_initialize(), which still runs inside nx_start()
 *   on this port, so up_mdelay()/usleep() can wait for a tick that cannot
 *   arrive yet.
 *
 ****************************************************************************/

static void p4_busy_delay(uint32_t msec)
{
  volatile uint32_t n = msec * 20000u;

  while (n-- > 0)
    {
      __asm__ volatile("nop");
    }
}

static int board_display_gpio_initialize(void)
{
  int ret;

  esp_gpiowrite(BOARD_LCD_BACKLIGHT_GPIO, false);
  esp_gpio_matrix_out(BOARD_LCD_BACKLIGHT_GPIO, SIG_GPIO_OUT_IDX, false,
                      false);
  ret = esp_configgpio(BOARD_LCD_BACKLIGHT_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight GPIO%d config failed: %d\n",
             BOARD_LCD_BACKLIGHT_GPIO, ret);
      return ret;
    }

  /* Probe A: plain level write on the reset pin. */

  esp_gpiowrite(BOARD_LCD_RESET_GPIO, false);

  /* Configure the reset pin as an output using the HAL directly.
   *
   * esp_configgpio(pin, OUTPUT) also clears the internal pull-up, and that
   * helper polls with a 1 ms timeout which never completes for this pin on the
   * current port, so it is skipped here: the pad is driven by the panel reset
   * net, not by the internal pull-up.
   */

  ret = esp_configgpio(BOARD_LCD_RESET_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: reset GPIO%d config failed: %d\n",
             BOARD_LCD_RESET_GPIO, ret);
      return ret;
    }

  p4_busy_delay(10);
  esp_gpiowrite(BOARD_LCD_RESET_GPIO, true);
  p4_busy_delay(20);
  return OK;
}

/****************************************************************************
 * Name: board_display_write
 *
 * Description:
 *   Write one DCS parameter to the panel on virtual channel 0.
 *
 ****************************************************************************/

static int board_display_write(uint8_t command, uint8_t value)
{
  return esp32p4_mipi_dsi_write_dcs(0, command, &value, 1);
}

/****************************************************************************
 * Name: board_ek79007_initialize
 *
 * Description:
 *   Run the EK79007 power-on sequence while the controller is still in
 *   command mode, then leave the panel in sleep-out.
 *
 ****************************************************************************/

static int board_ek79007_initialize(void)
{
  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(g_ek79007_init) / sizeof(g_ek79007_init[0]); i++)
    {
      ret = board_display_write(g_ek79007_init[i].command,
                                g_ek79007_init[i].value);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: panel cmd 0x%02x failed: %d\n",
                 g_ek79007_init[i].command, ret);
          return ret;
        }

      p4_busy_delay(1);
    }

  ret = esp32p4_mipi_dsi_write_dcs(0, 0x11, NULL, 0);  /* Sleep out */
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: sleep-out failed: %d\n", ret);
      return ret;
    }

  p4_busy_delay(BOARD_LCD_DCS_SETTLE_MS);
  return OK;
}

/****************************************************************************
 * Name: board_display_clear
 *
 * Description:
 *   Publish a complete frame before video/backlight start, so the panel never
 *   shows whatever happened to be in the framebuffer.
 *
 ****************************************************************************/

static int board_display_clear(FAR struct fb_vtable_s *fb)
{
  struct fb_planeinfo_s pinfo;
  int ret;

  ret = fb->getplaneinfo(fb, 0, &pinfo);
  if (ret < 0)
    {
      return ret;
    }

  if (pinfo.fbmem == NULL)
    {
      return -ENOMEM;
    }

  memset(pinfo.fbmem, 0, pinfo.fblen);
  return fb->pandisplay(fb, &pinfo);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_display_initialize
 *
 * Description:
 *   Bring up the EK79007 panel and register /dev/fb0.  Called from
 *   board_late_initialize().
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int board_display_initialize(void)
{
  const struct esp32p4_dsi_config_s config =
  {
    .num_lanes = 2,
    .lane_bit_rate_mbps = 700,   /* P4X-DSIRATE: 1000 was far above what
                                  * 48 MHz RGB565 needs (2 lanes x 384 Mbps) and
                                  * left no margin on the flex - touching the
                                  * ribbon dropped the link (blue screen). */
    .color_format = ESP32P4_DSI_COLOR_RGB565,
    .timing =
    {
      .hsw = 10,
      .hbp = 120,
      .width = BOARD_LCD_WIDTH,
      .hfp = 120,
      .vsw = 1,
      .vbp = 20,
      .height = BOARD_LCD_HEIGHT,
      .vfp = 10,
      .dpi_clk_mhz = 48,
    },
  };
  FAR struct fb_vtable_s *fb;
  int ret;

  ret = board_display_gpio_initialize();
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_mipi_dsi_initialize(&config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI host init failed: %d\n", ret);
      return ret;
    }

  ret = board_ek79007_initialize();
  if (ret < 0)
    {
      return ret;
    }

  fb = esp32p4_fb_initialize(0);
  if (fb == NULL)
    {
      syslog(LOG_ERR, "ERROR: esp32p4_fb_initialize failed\n");
      return -ENOMEM;
    }

  ret = board_display_clear(fb);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: initial frame failed: %d\n", ret);
      return ret;
    }

  ret = esp32p4_mipi_dsi_start_video();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: start_video failed: %d\n", ret);
      return ret;
    }

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      /* The framebuffer may already have been registered by the generic
       * video layer; that is not fatal.
       */

      syslog(LOG_WARNING, "WARNING: fb_register(0,0) returned %d\n", ret);
    }

  esp_gpiowrite(BOARD_LCD_BACKLIGHT_GPIO, true);

  syslog(LOG_INFO, "ESP32-P4X: EK79007 ready: /dev/fb0 %dx%d RGB565\n",
         BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
  return OK;
}

/****************************************************************************
 * Name: board_display_uninitialize
 ****************************************************************************/

void board_display_uninitialize(void)
{
  esp_gpiowrite(BOARD_LCD_BACKLIGHT_GPIO, false);
  esp32p4_mipi_dsi_stop_video();
}

#endif /* CONFIG_ESP32P4X_DISPLAY */

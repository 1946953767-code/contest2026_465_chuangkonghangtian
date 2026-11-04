/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4x-function-ev-board/src/esp32p4x_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mm/mm.h>
#include <nuttx/video/fb.h>
/* P4X-NATIVE-NAME: exported so the vendor board driver can delegate to the
 * official openvela implementation instead of duplicating it.
 */
int esp32p4_native_fb_initialize(int display);
FAR struct fb_vtable_s *esp32p4_native_fb_getvplane(int display, int plane);

#include <nuttx/video/mipi_dsi.h>

#include <arch/chip/gpio_sig_map.h>
#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_ldo.h"
#include "esp_ldo_regulator.h"
#include "espressif/esp_mipi_dsi.h"
#include "hal/cache_ll.h"

#include "esp32p4x_display.h"

#ifdef CONFIG_ESP32P4X_LCD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP32P4X_FB_SIZE (ESP32P4X_LCD_STRIDE * ESP32P4X_LCD_HEIGHT)

#define ESP32P4X_PANEL_NAME        "ek79007"
#define ESP32P4X_DCS_SETTLE_MS     120
#define ESP32P4X_TEST_COLOR        0x001f  /* Blue: an unlit panel is obvious */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32p4x_panel_cmd_s
{
  uint8_t cmd;
  uint8_t value;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* EK79007 initialization table, taken from the locally verified bring-up
 * sequence for this exact panel.
 */

static const struct esp32p4x_panel_cmd_s g_ek79007_init[] =
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

static FAR uint16_t *g_fb;    /* cached view (heap bookkeeping) */
static FAR uint16_t *g_fb_nc; /* non-cacheable alias: CPU + DSI DMA */
static bool g_fb_ready;

static const struct fb_videoinfo_s g_video =
{
  .fmt     = FB_FMT_RGB16_565,
  .xres    = ESP32P4X_LCD_WIDTH,
  .yres    = ESP32P4X_LCD_HEIGHT,
  .nplanes = 1,
};

/* P4X-DSIRATE: the panel lost its DSI link repeatedly at the default rate, and
 * a lower lane bitrate gives the FPC/connector more signal margin.  P4X_DSI_MBPS
 * overrides the compiled default at boot for an A/B check.
 */

static int p4x_dsi_bitrate(void)
{
  FAR char *env = getenv("P4X_DSI_MBPS");
  int mbps = (env != NULL) ? atoi(env) : 0;

  if (mbps >= 100 && mbps <= 1500)
    {
      syslog(LOG_INFO, "P4X-DSIRATE: using %d Mbps (P4X_DSI_MBPS)\n", mbps);
      return mbps;
    }

  return ESP32P4X_LCD_DSI_BITRATE_MBPS;
}

static struct fb_planeinfo_s g_plane =
{
  .fbmem        = NULL,
  .fblen        = ESP32P4X_FB_SIZE,
  .stride       = ESP32P4X_LCD_STRIDE,
  .display      = 0,
  .bpp          = ESP32P4X_LCD_BPP,
  .xres_virtual = ESP32P4X_LCD_WIDTH,
  .yres_virtual = ESP32P4X_LCD_HEIGHT,
  .xoffset      = 0,
  .yoffset      = 0,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4x_fb_release
 *
 * Description:
 *   Return the framebuffer to the heap it came from.
 *
 ****************************************************************************/

static void esp32p4x_fb_release(void)
{
  if (g_fb == NULL)
    {
      return;
    }

  mm_free(g_mmheap, g_fb);
  g_fb_nc = NULL;
}


/****************************************************************************
 * Name: esp32p4x_display_gpio_initialize
 *
 * Description:
 *   Configure the panel reset and backlight GPIOs and release reset.
 *
 ****************************************************************************/

/* The MIPI D-PHY analog supply (VDD_MIPI_DPHY) comes from on-chip LDO
 * channel 3.  esp_mipi_dsi_initialize() deliberately does not power it, so
 * board code must do it first, otherwise the PHY PLL never locks and the host
 * init returns -ETIMEDOUT.
 */

#define ESP32P4X_MIPI_PHY_LDO_CHAN       3
#define ESP32P4X_MIPI_PHY_LDO_VOLTAGE_MV 2500

static struct esp_ldo_config_t g_mipi_phy_ldo =
{
  .chan_id    = ESP32P4X_MIPI_PHY_LDO_CHAN,
  .voltage_mv = ESP32P4X_MIPI_PHY_LDO_VOLTAGE_MV,
  .handler    = NULL,
};

/****************************************************************************
 * Name: esp32p4x_mipi_phy_power
 *
 * Description:
 *   Turn the on-chip LDO that feeds VDD_MIPI_DPHY on or off.
 *
 ****************************************************************************/

static int esp32p4x_mipi_phy_power(bool on)
{
  if (on)
    {
      esp_ldo_channel_config_t cfg;
      esp_ldo_channel_handle_t chan = NULL;
      esp_err_t err;

      memset(&cfg, 0, sizeof(cfg));
      cfg.chan_id    = g_mipi_phy_ldo.chan_id;
      cfg.voltage_mv = g_mipi_phy_ldo.voltage_mv;


      err = esp_ldo_acquire_channel(&cfg, &chan);


      if (err != ESP_OK)
        {
          return -EIO;
        }

      g_mipi_phy_ldo.handler = (void *)chan;
      return OK;
    }

  return OK;
}

/****************************************************************************
 * Name: p4x_busy_delay
 *
 * Description:
 *   Millisecond delay that does NOT depend on the system tick.  This board
 *   driver runs from board_late_initialize(), which still executes inside
 *   nx_start(); p4x_busy_delay() waits for the scheduler tick and spins forever
 *   there (confirmed over JTAG: the core was parked at up_mdelay+0x28).
 *   The CPU is already at its final 400 MHz, so a calibrated loop is fine for
 *   the sub-30 ms panel power-on timings.
 *
 ****************************************************************************/

static void p4x_busy_delay(uint32_t msec)
{
  /* up_mdelay() itself is a calibrated busy loop on this port
   * (CONFIG_BOARD_LOOPSPERMSEC), so it needs no scheduler tick and gives the
   * real millisecond timing the panel reset sequence requires.
   */

  up_mdelay(msec);
}

static int esp32p4x_display_gpio_initialize(void)
{
  int ret;

  /* Backlight off first, so the panel never lights before it has a frame. */

  esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, false);
  esp_gpio_matrix_out(ESP32P4X_LCD_BACKLIGHT_GPIO, SIG_GPIO_OUT_IDX,
                      false, false);
  ret = esp_configgpio(ESP32P4X_LCD_BACKLIGHT_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight GPIO%d config failed: %d\n",
             ESP32P4X_LCD_BACKLIGHT_GPIO, ret);
      return ret;
    }

  /* Panel reset: assert, hold, then release. */

  esp_gpiowrite(ESP32P4X_LCD_RESET_GPIO, false);
  esp_gpio_matrix_out(ESP32P4X_LCD_RESET_GPIO, SIG_GPIO_OUT_IDX,
                      false, false);
  ret = esp_configgpio(ESP32P4X_LCD_RESET_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: reset GPIO%d config failed: %d\n",
             ESP32P4X_LCD_RESET_GPIO, ret);
      return ret;
    }

  p4x_busy_delay(10);
  esp_gpiowrite(ESP32P4X_LCD_RESET_GPIO, true);
  p4x_busy_delay(20);
  return OK;
}

/****************************************************************************
 * Name: esp32p4x_panel_initialize
 *
 * Description:
 *   Register the EK79007 DSI device on the host, attach it, and run the
 *   initialization table plus sleep-out.  Command mode is active here,
 *   before video streaming starts.
 *
 ****************************************************************************/

static FAR struct mipi_dsi_device *
esp32p4x_panel_initialize(FAR struct mipi_dsi_host *host)
{
  FAR struct mipi_dsi_device *device;
  unsigned int i;
  int ret;

  device = mipi_dsi_device_register(host, ESP32P4X_PANEL_NAME, 0);
  if (device == NULL)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_device_register failed\n");
      return NULL;
    }

  device->lanes      = ESP32P4X_LCD_DSI_LANES;
  device->format     = MIPI_DSI_FMT_RGB565;
  device->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
                       MIPI_DSI_MODE_LPM;
  device->hs_rate    = ESP32P4X_LCD_DSI_BITRATE_MBPS * 1000000UL;
  device->lp_rate    = 0;

  ret = mipi_dsi_attach(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_attach failed: %d\n", ret);
      return NULL;
    }

  for (i = 0; i < sizeof(g_ek79007_init) / sizeof(g_ek79007_init[0]); i++)
    {
      ret = mipi_dsi_dcs_write(device, g_ek79007_init[i].cmd,
                               &g_ek79007_init[i].value, 1);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: panel cmd 0x%02x failed: %d\n",
                 g_ek79007_init[i].cmd, ret);
          return NULL;
        }

      p4x_busy_delay(1);
    }

  ret = mipi_dsi_dcs_exit_sleep_mode(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: exit_sleep_mode failed: %d\n", ret);
      return NULL;
    }

  p4x_busy_delay(ESP32P4X_DCS_SETTLE_MS);
  return device;
}

/****************************************************************************
 * Name: esp32p4x_fb_fill
 *
 * Description:
 *   Fill the framebuffer with a solid color and push it out over DSI.
 *
 ****************************************************************************/

static void esp32p4x_fb_fill(uint16_t color)
{
  size_t i;
  size_t npix = (size_t)ESP32P4X_LCD_WIDTH * ESP32P4X_LCD_HEIGHT;

  if (g_fb == NULL)
    {
      return;
    }

  for (i = 0; i < npix; i++)
    {
      g_fb[i] = color;                       /* cached buffer */
    }

  /* P4X-CACHED-FB: write the whole frame back so the DW-GDMA sees it. */

  esp_mipi_dsi_flush_framebuffer(g_fb, ESP32P4X_FB_SIZE);
}

/****************************************************************************
 * Name: esp32p4x_getvideoinfo
 ****************************************************************************/

static int esp32p4x_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                 FAR struct fb_videoinfo_s *vinfo)
{
  memcpy(vinfo, &g_video, sizeof(*vinfo));
  return OK;
}

/****************************************************************************
 * Name: esp32p4x_getplaneinfo
 ****************************************************************************/

static int esp32p4x_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                 FAR struct fb_planeinfo_s *pinfo)
{
  if (planeno != 0 || g_plane.fbmem == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &g_plane, sizeof(*pinfo));
  return OK;
}

#ifdef CONFIG_FB_UPDATE
/****************************************************************************
 * Name: esp32p4x_updatearea
 ****************************************************************************/

static int esp32p4x_updatearea(FAR struct fb_vtable_s *vtable,
                               FAR const struct fb_area_s *area)
{
  size_t offset;
  size_t len;

  if (g_fb == NULL)
    {
      return -EAGAIN;
    }

  if (area == NULL)
    {
      return esp_mipi_dsi_flush_framebuffer(g_fb, ESP32P4X_FB_SIZE);
    }

  if (area->x >= ESP32P4X_LCD_WIDTH || area->y >= ESP32P4X_LCD_HEIGHT)
    {
      return OK;
    }

  offset = (size_t)area->y * ESP32P4X_LCD_STRIDE +
           (size_t)area->x * (ESP32P4X_LCD_BPP / 8);
  len = (size_t)area->h * ESP32P4X_LCD_STRIDE;
  if (offset + len > ESP32P4X_FB_SIZE)
    {
      len = ESP32P4X_FB_SIZE - offset;
    }

  /* P4X-CACHED-FB: push the dirty region out to physical memory so the DMA sees
   * the new pixels.
   */

  return esp_mipi_dsi_flush_framebuffer((FAR uint8_t *)g_fb + offset, len);
}
#endif /* CONFIG_FB_UPDATE */

static struct fb_vtable_s g_vtable =
{
  .getvideoinfo = esp32p4x_getvideoinfo,
  .getplaneinfo = esp32p4x_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
  .updatearea   = esp32p4x_updatearea,
#endif
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4x_display_initialize
 ****************************************************************************/

int esp32p4x_display_initialize(void)
{
  struct esp_mipi_dsi_bus_config_s buscfg =
    {
      .num_data_lanes     = ESP32P4X_LCD_DSI_LANES,
      .lane_bit_rate_mbps = p4x_dsi_bitrate(),
    };
  int ret;

  /* EXPERIMENT: the GPIO step parks the CPU on this board (stage marker
   * stays at 1 across three independent builds), so it is bypassed while the
   * rest of the panel chain is validated.  The backlight is on by default on
   * this board, and the DSI host drives the panel reset itself.
   */

  /* Panel reset (GPIO27 -> LCD adapter J6 RST_LCD) and backlight enable
   * (GPIO26 -> J6 PWM), per the official ESP32-P4X-Function-EV-Board user
   * guide.  This step only ever "parked the CPU" while the interrupt storm and
   * the bad ROM soft-float call were still present; with those fixed it must
   * run, otherwise the panel is held in reset and every DCS command times out
   * with -ETIMEDOUT.
   */

  ret = esp32p4x_display_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: panel GPIO init failed: %d\n", ret);
      return ret;
    }

  /* Power VDD_MIPI_DPHY before touching the DSI host. */

  ret = esp32p4x_mipi_phy_power(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI PHY LDO enable failed: %d\n", ret);
      return ret;
    }

  p4x_busy_delay(10);

  ret = esp_mipi_dsi_initialize(&buscfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI host init failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Bring up the EK79007 panel and register /dev/fb0.  Called by the
 *   generic framebuffer layer when CONFIG_DRIVERS_VIDEO is enabled.
 *
 ****************************************************************************/

#define P4X_GDMA3 (0x50081100u)

static void p4x_probe(const char *tag)
{
  FAR volatile uint32_t *p = (FAR volatile uint32_t *)P4X_GDMA3;

  syslog(LOG_INFO, "P4X-STEPLOG %s: ctl0=%08x ctl1=%08x llp=%08x sts=%08x "
         "chen=%08x\n", tag,
         (unsigned int)p[0], (unsigned int)p[1], (unsigned int)p[2],
         (unsigned int)p[6], (unsigned int)*(FAR volatile uint32_t *)0x50081040);
}

int up_fbinitialize(int display)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *device;
  struct esp_mipi_dsi_dpi_config_s dpi;
  struct esp32p4_dsi_config_s cfg;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_fb_ready)
    {
      return OK;
    }

  /* IDF HAL stack: initialise the host, PHY PLL, bridge, the EK79007 panel
   * sequence and the DPI timing in one call.
   */

  memset(&cfg, 0, sizeof(cfg));
  cfg.num_lanes          = ESP32P4X_LCD_DSI_LANES;
  cfg.lane_bit_rate_mbps = (float)p4x_dsi_bitrate();
  cfg.color_format       = ESP32P4_DSI_COLOR_RGB565;
  cfg.timing.hsw         = ESP32P4X_LCD_HSW;
  cfg.timing.hbp         = ESP32P4X_LCD_HBP;
  cfg.timing.width       = ESP32P4X_LCD_WIDTH;
  cfg.timing.hfp         = ESP32P4X_LCD_HFP;
  cfg.timing.vsw         = ESP32P4X_LCD_VSW;
  cfg.timing.vbp         = ESP32P4X_LCD_VBP;
  cfg.timing.height      = ESP32P4X_LCD_HEIGHT;
  cfg.timing.vfp         = ESP32P4X_LCD_VFP;
  cfg.timing.dpi_clk_mhz = (float)ESP32P4X_LCD_DPI_CLK_MHZ;

  /* P4X-PANELRETRY: on a cold boot the DSI PHY/PLL may not be locked yet, and a
   * single failed attempt leaves the board with no /dev/fb0 at all - the panel
   * then only shows its backlight.  Give the rails 300 ms and retry the whole
   * init a few times before giving up.
   */

  p4x_busy_delay(300);

  {
    int tries;

    for (tries = 0; tries < 3; tries++)
      {
        ret = esp32p4_mipi_dsi_initialize(&cfg);
        if (ret >= 0)
          {
            break;
          }

        syslog(LOG_ERR, "ERROR: IDF DSI init failed: %d (try %d)\n", ret,
               tries + 1);
        p4x_busy_delay(250);
      }
  }

  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: IDF DSI init failed for good: %d\n", ret);
      return ret;
    }

  /* Cached framebuffer in PSRAM (24 MB user heap), matching the reference. */

  g_fb = mm_memalign(g_mmheap, 64, ESP32P4X_FB_SIZE);   /* PSRAM global heap */
  if (g_fb == NULL)
    {
      syslog(LOG_ERR, "ERROR: framebuffer alloc failed\n");
      return -ENOMEM;
    }

  memset(g_fb, 0, ESP32P4X_FB_SIZE);
  esp_mipi_dsi_flush_framebuffer((FAR uint8_t *)g_fb, ESP32P4X_FB_SIZE);
  g_plane.fbmem = (FAR uint8_t *)g_fb;

  ret = esp32p4_mipi_dsi_set_framebuffer((uintptr_t)g_fb, ESP32P4X_FB_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: set_framebuffer failed: %d\n", ret);
      goto errout;
    }

  dpi.h_res              = ESP32P4X_LCD_WIDTH;
  dpi.v_res              = ESP32P4X_LCD_HEIGHT;
  dpi.hsync_pulse_width  = ESP32P4X_LCD_HSW;
  dpi.hsync_back_porch   = ESP32P4X_LCD_HBP;
  dpi.hsync_front_porch  = ESP32P4X_LCD_HFP;
  dpi.vsync_pulse_width  = ESP32P4X_LCD_VSW;
  dpi.vsync_back_porch   = ESP32P4X_LCD_VBP;
  dpi.vsync_front_porch  = ESP32P4X_LCD_VFP;
  dpi.dpi_clock_freq_mhz = ESP32P4X_LCD_DPI_CLK_MHZ;
  dpi.virtual_channel    = 0;
  dpi.format             = MIPI_DSI_FMT_RGB565;
  (void)esp_mipi_dsi_configure_dpi(&dpi);

  host = esp_mipi_dsi_host_get();
  if (host != NULL)
    {
      device = esp32p4x_panel_initialize(host);
      (void)device;
    }

  ret = esp32p4_mipi_dsi_start_video();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: IDF start_video failed: %d\n", ret);
      goto errout;
    }

  esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, true);

  g_fb_ready = true;
  syslog(LOG_INFO, "ESP32-P4X: EK79007 up (IDF stack): /dev/fb0 %dx%d RGB565\n",
         ESP32P4X_LCD_WIDTH, ESP32P4X_LCD_HEIGHT);
  return OK;

errout:
  mm_free(g_mmheap, g_fb);
  g_fb = NULL;
  g_plane.fbmem = NULL;
  return ret;
}

/****************************************************************************
 * Name: esp32p4x_panel_reinit
 *
 * Description:
 *   P4X-PANELKICK: replay the whole panel bring-up at run time.
 *
 *   A cold power-up can leave the glass dark while the backlight is driven -
 *   the panel's own rails were apparently not settled when the board initialised
 *   it - and a warm reset always fixes it.  This replays exactly what the warm
 *   path does: reset pulse, DCS init, video start, backlight on.
 *
 ****************************************************************************/

int esp32p4x_panel_reinit(void)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *device;

  host = esp_mipi_dsi_host_get();
  if (host == NULL)
    {
      return -ENODEV;
    }

  esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, false);
  esp_gpiowrite(ESP32P4X_LCD_RESET_GPIO, false);
  p4x_busy_delay(30);
  esp_gpiowrite(ESP32P4X_LCD_RESET_GPIO, true);
  p4x_busy_delay(150);

  device = esp32p4x_panel_initialize(host);
  if (device == NULL)
    {
      /* P4X-KICKOFF: never leave the panel dark on the error path - the
       * backlight was switched off a few lines above.
       */

      esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, true);
      syslog(LOG_ERR, "ERROR: P4X panel reinit failed\n");
      return -EIO;
    }

  (void)esp32p4_mipi_dsi_start_video();

  if (g_fb != NULL)
    {
      esp_mipi_dsi_flush_framebuffer((FAR uint8_t *)g_fb, ESP32P4X_FB_SIZE);
    }

  esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, true);
  syslog(LOG_INFO, "ESP32-P4X: panel re-initialised\n");
  return OK;
}

/****************************************************************************
 * Name: up_fbgetvplane
 ****************************************************************************/

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0)
    {
      return NULL;
    }

  return &g_vtable;
}

/****************************************************************************
 * Name: up_fbuninitialize
 ****************************************************************************/

void up_fbuninitialize(int display)
{
  if (display != 0)
    {
      return;
    }

  esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, false);
  esp_mipi_dsi_video_stop();

  if (g_fb != NULL)
    {
      esp32p4x_fb_release();
      g_fb = NULL;
    }

  g_plane.fbmem = NULL;
  g_fb_ready = false;
}

#endif /* CONFIG_ESP32P4X_LCD */

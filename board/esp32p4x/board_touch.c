/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_touch.c
 *
 * GT911 capacitive touch, polled directly by the board.
 *
 * Wiring (official ESP-BSP for this exact board):
 *   I2C0  SDA = GPIO7, SCL = GPIO8.  The controller sits on the LCD module and
 *   there is no reset / interrupt GPIO (BSP_LCD_TOUCH_RST/INT = NC).
 *
 * Why this file does not use the NuttX input driver:
 *   the GT9xx driver expects an interrupt to announce samples (none is wired)
 *   and its read() blocks for up to the I2C timeout when only one point is
 *   reported, which froze the UI (errno 110).  The controller protocol is
 *   simple and was verified over I2C, so the board polls it instead:
 *
 *     0x814E status  : bit7 = buffer ready, bits[3:0] = contact count
 *                      MUST be written back to 0 or the flag never re-arms
 *     0x8150 point   : [id][x_l][x_h][y_l][y_h][size_l][size_h]
 *
 * The last sample is published through board_touch_state().
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/clock.h>
#include <nuttx/kthread.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c.h"

#include "board_touch.h"

#ifdef CONFIG_INPUT_GT9XX

#define GT911_ADDR        0x5d
#define GT911_REG_STATUS  0x814e
#define GT911_REG_POINT1  0x8150

static FAR struct i2c_master_s *g_i2c;
static struct i2c_config_s g_cfg;
static volatile int g_x = -1;
static volatile int g_y = -1;
static volatile int g_pressed;
static volatile int g_idle;   /* release debounce */
static volatile unsigned long g_press_tick;   /* P4X-RELEASE2: last contact */
static volatile int g_last_id = -1;           /* P4X-RELEASE2: packet id */

/* P4X-DEBOUNCE2: a press is only reported after two samples agree on the
 * position.  The controller emits the occasional ghost contact; a single noisy
 * sample used to be enough to make a marker appear out of nowhere.
 */

static int g_pend_x = -1;
static int g_pend_y = -1;
static int g_pend_count;

/* P4X-MAP: the panel reports mirrored axes, so expose the raw point together
 * with a transformed one; the transform is switchable at runtime so the right
 * variant can be found without rebuilding.
 *   0 none | 1 mirror x | 2 mirror y | 3 mirror both | 4 swap
 *   5 swap+mirror x | 6 swap+mirror y | 7 swap+mirror both
 */

static volatile int g_map = 3;   /* P4X-MIRROR: x+y mirror (180 deg) */
static volatile int g_enabled = 1;   /* P4X-ENABLE */

/* P4X-KEEP-DECL: prototypes so the calls below compile. */
FAR void *board_bigalloc(size_t size, size_t align);
void board_bigfree(FAR void *p);

/* P4X-BIGALLOC: the 32 MB PSRAM heap (g_mmheap) is where the framebuffer comes
 * from; apps cannot reach it with malloc(), so expose it here.
 */

#include <nuttx/mm/mm.h>

extern FAR struct mm_heap_s *g_mmheap;

static int g_raw_x, g_raw_y;
static volatile unsigned long g_reports;
static volatile unsigned long g_release_count;   /* P4X-EDGE */

/* P4X-GESTURE: page flips are decided here, inside the polling task, so the
 * UI does not have to wait for the next (slow) render tick to notice a swipe.
 *   g_gx0/g_gy0  - where this touch started (mapped coordinates)
 *   g_gdone      - one flip per touch, so a long drag cannot skip pages
 */

/* P4X-SETTLE: gesture tuning.
 *   SETTLE_TICKS - pressed samples to ignore before the anchor is trusted
 *   MIN_TICKS    - minimum contact time before a page may turn
 *   SWIPE_PX     - required excursion from the anchor
 */

#define GEST_SWIPE_PX      60    /* P4X-QUIET2: easier to trigger */
#define SWIPE_SETTLE_TICKS 2
#define SWIPE_MIN_TICKS    6

static volatile int g_gx0 = -1;   /* P4X-DIR: mapped anchor */
static volatile int g_gy0 = -1;
static volatile int g_gdone;
static volatile int g_flip_pending;
static volatile int g_flip_dir;
static volatile unsigned long g_flip_count;
static volatile unsigned long g_flip_tick;

/* P4X-NOSWIPE: the UI marks the on-screen button rectangle before the finger
 * lands; swipes starting inside it are ignored, so tapping the button does not
 * also turn the page.
 */

static volatile int g_nx, g_ny;      /* P4X-DWELL: mapped current point */

/* P4X-SETTLE: per-gesture state.  The first samples after touch-down carry
 * the controller's wake-up jitter, so the anchor is the average of the two
 * samples around the settle point, and the direction comes from the largest
 * excursion seen on either side rather than from one sample-to-sample delta.
 */

static volatile unsigned long g_gtick;
static volatile int g_gsamples;
static volatile int g_anchor_x;
static volatile int g_anchor_y;
static volatile int g_max_left;
static volatile int g_max_right;
static volatile int g_ns_x0 = -1;
static volatile int g_ns_y0 = -1;
static volatile int g_ns_x1 = -1;
static volatile int g_ns_y1 = -1;

static int gt911_read(uint16_t reg, FAR uint8_t *buf, int len)
{
  uint8_t r[2];

  r[0] = reg >> 8;
  r[1] = reg & 0xff;
  return i2c_writeread(g_i2c, &g_cfg, r, 2, buf, len);
}

static int gt911_write(uint16_t reg, uint8_t val)
{
  uint8_t r[3];

  r[0] = reg >> 8;
  r[1] = reg & 0xff;
  r[2] = val;
  return i2c_write(g_i2c, &g_cfg, r, 3);
}

/****************************************************************************
 * Name: board_touch_sampler
 *
 * Description:
 *   Poll the controller and publish the newest sample.
 *
 ****************************************************************************/

static int board_touch_sampler(int argc, FAR char *argv[])
{
  uint8_t st;
  uint8_t pt[8];

  /* Clear any stale ready flag so the controller can re-arm it. */

  gt911_write(GT911_REG_STATUS, 0);

  for (;;)
    {
      /* Read the whole packet starting at the status register: this module
       * returns [status][id][x_lsb][x_msb][y_lsb][y_msb][size_l][size_h].
       */

      if (gt911_read(GT911_REG_STATUS, pt, sizeof(pt)) == OK)
        {
          st = pt[0];

          /* Only the contact count decides pressed/released: bit 7 is not set
           * on every sample this module returns (measured status=0x01 with one
           * contact), and treating that as a release turned one swipe into
           * several page changes.
           */

          /* P4X-RELEASE2: a report only counts when the module flags it as
           * fresh (the id byte changes per packet) and the value is sane.
           * Stale reads used to look like a live contact.
           */

          /* P4X-DEBOUNCE2: sanity filter + two-sample confirmation. */

          if ((st & 0x0f) >= 1 &&
              (pt[2] | (pt[3] << 8)) < 1024 &&
              (pt[4] | (pt[5] << 8)) < 600 &&
              !((pt[2] | (pt[3] << 8)) == 0 && (pt[4] | (pt[5] << 8)) == 0))
            {
              int px = pt[2] | (pt[3] << 8);
              int py = pt[4] | (pt[5] << 8);
              int sx = g_ns_x0, sy = g_ns_y0, ex = g_ns_x1, ey = g_ns_y1;
              int nx, ny;

              /* P4X-REVERT: two-sample confirmation removed (it delayed every
               * touch by one sample and the ghost contacts it targeted were
               * the marker drawing, not real samples).
               */

              (void)px;
              (void)py;

              nx = px;
              ny = py;

              g_last_id = pt[1];
              g_press_tick = clock_systime_ticks();
              g_raw_x = pt[2] | (pt[3] << 8);
              g_raw_y = pt[4] | (pt[5] << 8);
              g_x = g_raw_x;
              g_y = g_raw_y;
              g_pressed = 1;
              g_idle = 0;
              g_reports++;

              /* P4X-GESTURE: decide the horizontal swipe as soon as the finger
               * has travelled far enough - do not wait for the lift.
               */

              /* P4X-DWELL: mapped current point, used by the button test. */

              (void)nx;
              (void)ny;
              g_nx = (g_map & 1) ? 1023 - g_x : g_x;
              g_ny = (g_map & 2) ? 599 - g_y : g_y;

              /* P4X-SETTLE: the first pressed sample only starts the
               * gesture; its coordinate is not trusted yet.
               */

              if (g_gx0 < 0)
                {
                  g_gtick = clock_systime_ticks();
                  g_gsamples = 1;
                  g_gx0 = g_nx;
                  g_gy0 = g_ny;
                  g_max_left = 0;
                  g_max_right = 0;
                }
              else
                {
                  g_gsamples++;

                  if (g_gsamples == SWIPE_SETTLE_TICKS)
                    {
                      /* Two clean samples define the anchor.  P4X-NOSWIPE:
                       * the mapped point decides, because the app's button
                       * rectangle is expressed in mapped coordinates.
                       */

                      g_anchor_x = (g_gx0 + g_nx) / 2;
                      g_anchor_y = (g_gy0 + g_ny) / 2;
                      g_gx0 = g_anchor_x;
                      g_gy0 = g_anchor_y;

                      if (sx >= 0 && g_anchor_x >= sx && g_anchor_x <= ex &&
                          g_anchor_y >= sy && g_anchor_y <= ey)
                        {
                          g_gdone = 1;      /* started on the on-screen button */
                        }
                    }
                  else if (g_gsamples > SWIPE_SETTLE_TICKS && !g_gdone)
                    {
                      int dx = g_nx - g_anchor_x;

                      if (dx < 0 && -dx > g_max_left)
                        {
                          g_max_left = -dx;
                        }
                      else if (dx > 0 && dx > g_max_right)
                        {
                          g_max_right = dx;
                        }

                      /* P4X-DWELL: never page while the finger sits on the
                       * button, and drop anything already queued.
                       */

                      if ((g_anchor_x >= sx && g_anchor_x <= ex &&
                           g_anchor_y >= sy && g_anchor_y <= ey) ||
                          (g_nx >= sx && g_nx <= ex &&
                           g_ny >= sy && g_ny <= ey))
                        {
                          g_gdone = 1;
                          g_flip_pending = 0;
                        }
                      else
                        {
                          /* P4X-DIR2: the phone model - the content follows
                           * the finger, so a leftward swipe goes forward.
                           */

                          int dir = 0;
                          int travel = 0;

                          if (g_max_left >= GEST_SWIPE_PX &&
                              g_max_left > g_max_right)
                            {
                              dir = 1;
                              travel = g_max_left;
                            }
                          else if (g_max_right >= GEST_SWIPE_PX &&
                                   g_max_right > g_max_left)
                            {
                              dir = -1;
                              travel = g_max_right;
                            }

                          if (dir != 0 &&
                              (clock_systime_ticks() - g_gtick) >=
                              SWIPE_MIN_TICKS)
                            {
                              g_flip_dir = dir;
                              g_gdone = 1;
                              g_flip_pending = 1;
                              g_flip_count++;
                              g_flip_tick = clock_systime_ticks();
                              if (0)   /* P4X-QUIET-DIR: blocks the UI when no host reads */ syslog(LOG_INFO,
                                     "P4X-DIR: anchor %d now %d maxL=%d "
                                     "maxR=%d samples=%d travel=%d dir=%d\n",
                                     g_anchor_x, g_nx, g_max_left, g_max_right,
                                     g_gsamples, travel, dir);
                            }
                        }
                    }
                }
            }
          /* P4X-RELEASE2: confirm the lift after 120 ms with no new contact
           * report.  Counting zero-contact samples alone failed: one stale
           * report in between reset the counter and the release was lost, so
           * release-driven actions (the on-screen button) never ran.
           */

          if (g_pressed &&
              (clock_systime_ticks() - g_press_tick) >
              (TICK_PER_SEC / 8))
            {
              g_ns_x0 = -1;
              g_ns_y0 = -1;
              g_ns_x1 = -1;
              g_ns_y1 = -1;
              g_gx0 = -1;
              g_gy0 = -1;
              g_gsamples = 0;
              g_max_left = 0;
              g_max_right = 0;
              g_gdone = 0;
              g_last_id = -1;

              g_pressed = 0;
              g_idle = 0;
              g_pend_count = 0;
              g_release_count++;
            }

          /* Always clear the flag: without this the controller stops
           * refreshing the status register.
           */

          gt911_write(GT911_REG_STATUS, 0);
        }

      usleep(8000);    /* P4X-LAT: 40 ms was the main source of the lag */
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* P4X-BACKBUF: the DSI DMA scans the framebuffer continuously, so an app that
 * repaints only part of it can be caught mid-update ("torn" picture).  Provide a
 * off-screen buffer from the 32 MB PSRAM heap; the board owns it so the linker
 * keeps it, and apps draw into it and publish whole frames.
 */

static FAR uint16_t *g_bb;
static size_t g_bb_bytes;

/* P4X-FBFLUSH: the framebuffer is mapped cached, so CPU stores must be written
 * back before the DSI DMA can see them.  The driver does this in its
 * updatearea(); applications that touch the framebuffer directly need it too.
 */

extern int esp_mipi_dsi_flush_framebuffer(FAR void *addr, size_t len);

int board_fb_flush(FAR void *addr, size_t len)
{
  if (addr == NULL || len == 0)
    {
      return -EINVAL;
    }

  return esp_mipi_dsi_flush_framebuffer(addr, len);
}

FAR void *board_fb_backbuffer(size_t bytes)
{
  if (g_bb == NULL && bytes > 0)
    {
      g_bb = (FAR uint16_t *)mm_memalign(g_mmheap, 64, bytes);
      if (g_bb != NULL)
        {
          memset(g_bb, 0, bytes);
          g_bb_bytes = bytes;
          syslog(LOG_INFO, "P4X: back buffer %p (%u bytes)\n", g_bb,
                 (unsigned int)bytes);
        }
    }

  return g_bb;
}

int board_touch_initialize(void)
{

  uint8_t id[10];

  g_i2c = esp_i2cbus_initialize(ESPRESSIF_I2C0);
  if (g_i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: touch: I2C0 unavailable\n");
      return -ENODEV;
    }

  g_cfg.frequency = 400000;
  g_cfg.address   = GT911_ADDR;
  g_cfg.addrlen   = 7;

  if (gt911_read(0x8140, id, sizeof(id)) != OK)
    {
      syslog(LOG_WARNING, "WARNING: touch: no GT911 on I2C0 "
             "(SDA=GPIO%d, SCL=GPIO%d)\n",
             CONFIG_ESPRESSIF_I2C0_SDAPIN, CONFIG_ESPRESSIF_I2C0_SCLPIN);
      return -ENODEV;
    }

  syslog(LOG_INFO, "ESP32-P4X: GT911 \"%c%c%c%c\" res %dx%d (polled)\n",
         id[0], id[1], id[2], id[3],
         id[6] | (id[7] << 8), id[8] | (id[9] << 8));

  if (nxtask_create("tchsamp", 200, NULL, 2048, board_touch_sampler,
                    NULL, NULL) < 0)
    {
      syslog(LOG_ERR, "ERROR: touch: sampler task failed\n");
      return -ENOMEM;
    }

  return OK;
}

/****************************************************************************
 * Name: board_touch_gt911_cfg
 *
 * Description:
 *   Read or write the GT911 configuration table (0x8047..).  Bit 0 of the
 *   module-switch byte at 0x804D is the X mirror, bit 1 the Y mirror, bit 2
 *   the X/Y swap - the same controls the official driver exposes as
 *   flags.mirror_x / mirror_y.  Writing 1 to 0x8100 makes the controller
 *   re-read its config.
 *
 * Input Parameters:
 *   commit - non-zero to latch the change (write 0x8100 = 1)
 *
 ****************************************************************************/

int board_touch_gt911_cfg(FAR uint8_t *cfg, size_t len, int commit)
{
  if (g_i2c == NULL || cfg == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* Read-modify-write the module switch byte (0x804D). */

  if (gt911_read(0x804d, cfg, 1) != OK)
    {
      return -EIO;
    }

  return OK;
}

int board_touch_gt911_write(uint8_t modsw, int commit)
{
  uint8_t reg[2];

  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  reg[0] = 0x80;
  reg[1] = 0X4d;

  /* 0x804D = module switch 1 */

  {
    uint8_t buf[3];

    buf[0] = 0x80;
    buf[1] = 0x4d;
    buf[2] = modsw;
    if (i2c_write(g_i2c, &g_cfg, buf, 3) != OK)
      {
        return -EIO;
      }
  }

  if (commit)
    {
      uint8_t cb[3];

      cb[0] = 0x81;
      cb[1] = 0x00;
      cb[2] = 0x01;
      if (i2c_write(g_i2c, &g_cfg, cb, 3) != OK)
        {
          return -EIO;
        }
    }

  syslog(LOG_INFO, "P4X: GT911 module switch 0x804d = 0x%02x (commit %d)\n",
         modsw, commit);
  return OK;
}

void board_touch_enable(int on)
{
  /* P4X-KEEP: touch the allocator so the linker keeps it in the image. */

  {
    FAR void *probe = board_bigalloc(64, 64);

    if (probe != NULL)
      {
        board_bigfree(probe);
      }
  }

  g_enabled = on;
}

FAR void *board_bigalloc(size_t size, size_t align)
{
  return mm_memalign(g_mmheap, align ? align : 64, size);
}

void board_bigfree(FAR void *p)
{
  if (p != NULL)
    {
      mm_free(g_mmheap, p);
    }
}

/****************************************************************************
 * Name: board_touch_no_swipe
 *
 * Description:
 *   Mark a rectangle in mapped panel coordinates where a horizontal drag must
 *   not turn the page (the UI's on-screen button).  The zone is cleared when
 *   the finger lifts.
 *
 ****************************************************************************/

/* P4X-NOGEST2: the board no longer queues page flips.  Its gesture logic
 * double-fired with the UI's own rule (one swipe turned two pages), so the UI is
 * now the single place that decides a page change and this stays as a no-op.
 */

int board_touch_flip_queue_disabled(void)
{
  return 1;
}

void board_touch_no_swipe(int x0, int y0, int x1, int y1)
{
  g_flip_pending = 0;                /* P4X-DWELL: cancel a queued flip */
  g_ns_x0 = x0;
  g_ns_y0 = y0;
  g_ns_x1 = x1;
  g_ns_y1 = y1;
}

/****************************************************************************
 * Name: board_touch_flip
 *
 * Description:
 *   Return a swipe event, if the polling task recognised one, and clear it.
 *   dir is +1 for "next page" (finger moved left) and -1 for "previous page".
 *
 * Returned Value:
 *   One when an event was returned; zero when there is nothing to report.
 *
 ****************************************************************************/

int board_touch_flip(FAR int *dir)
{
  /* P4X-NOGEST2: the UI does the gesture handling; never report a flip. */

  g_flip_pending = 0;
  (void)dir;
  return 0;

  if (!g_flip_pending)
    {
      return 0;
    }

  g_flip_pending = 0;

  if (dir != NULL)
    {
      *dir = g_flip_dir;
    }

  return 1;
}

/****************************************************************************
 * Name: board_touch_flip_tick
 *
 * Description:
 *   System tick at which the last swipe was recognised - used to measure the
 *   end-to-end touch latency in the UI.
 *
 ****************************************************************************/

unsigned long board_touch_flip_tick(void)
{
  return g_flip_tick;
}

unsigned long board_touch_flip_count(void)
{
  return g_flip_count;
}

int board_touch_set_map(int map)
{
  g_map = map & 7;
  return OK;
}

/* P4X-BL: drive and read back the panel backlight (GPIO26 on this board).
 * The panel went dark while the UI kept publishing frames, which no log can
 * distinguish from a dead backlight - so make it a visible check.
 */

/* P4X-BLREG: real register readout of the backlight / panel reset pins.
 * esp_gpioread() returns 0 for a plain output pin (its input path is off), so
 * the decision "software never drove the pin" vs "pin is high and the panel
 * still stays dark" needs the output latch itself.
 */

#define P4X_GPIO_BASE   0x500e0000ul
#define P4X_GPIO_OUT    (P4X_GPIO_BASE + 0x04ul)
#define P4X_GPIO_ENABLE (P4X_GPIO_BASE + 0x20ul)
#define P4X_GPIO_STRAP  (P4X_GPIO_BASE + 0x1cul)
#define P4X_PIN_BL      26
#define P4X_PIN_RST     27

static unsigned int p4x_gpio_reg(unsigned long addr)
{
  return *(volatile unsigned int *)addr;
}

int board_backlight(int on)
{
  esp_gpiowrite(P4X_PIN_BL, on ? true : false);
  return (p4x_gpio_reg(P4X_GPIO_OUT) >> P4X_PIN_BL) & 1u;
}

void board_backlight_dump(const char *tag)
{
  unsigned int out = p4x_gpio_reg(P4X_GPIO_OUT);
  unsigned int en  = p4x_gpio_reg(P4X_GPIO_ENABLE);
  unsigned int st  = p4x_gpio_reg(P4X_GPIO_STRAP);

  printf("P4X-BLREG %s: OUT=0x%08x ENABLE=0x%08x STRAP=0x%08x "
         "| BL26 latch=%u out_en=%u pad=%u | RST27 latch=%u out_en=%u pad=%u\n",
         tag, out, en, st,
         (out >> P4X_PIN_BL) & 1u, (en >> P4X_PIN_BL) & 1u,
         (st >> P4X_PIN_BL) & 1u,
         (out >> P4X_PIN_RST) & 1u, (en >> P4X_PIN_RST) & 1u,
         (st >> P4X_PIN_RST) & 1u);
}

/* P4X-BLRST: replay the panel power-on reset: low, then released high. */

void board_panel_reset_pulse(void)
{
  esp_gpiowrite(P4X_PIN_RST, false);
  usleep(20000);
  esp_gpiowrite(P4X_PIN_RST, true);
  usleep(120000);
  board_backlight_dump("after reset pulse");
}

/****************************************************************************
 * Name: board_gpio_drive
 *
 * Description:
 *   P4X-PASCAN2: let an application drive a pad as a plain output.  The app
 *   cannot include the arch's esp_gpio.h, and this is only used by the speaker
 *   amplifier pin search.
 *
 ****************************************************************************/

/* P4X-GPIOIN: read a pad from application code.
 *
 * board_gpio_drive() below lets the application drive a pad; this is its input
 * twin.  The UI needs it for the BOOT key (the boot strap pad): the first
 * attempt read the GPIO input register by hand and always got the same value,
 * so the key looked dead.  Going through the HAL keeps the pad configuration
 * and the read in one place.
 */

int board_gpio_input(int pin, int pullup)
{
  gpio_pinattr_t attr;

  if (pin < 0 || pin > 54)
    {
      return -1;
    }

  attr = pullup ? (INPUT | PULLUP) : INPUT;
  esp_configgpio(pin, attr);
  return esp_gpioread(pin) ? 1 : 0;
}

int board_gpio_drive(int pin, int level)
{
  if (pin < 0 || pin > 54)
    {
      return -1;
    }

  if (esp_configgpio(pin, OUTPUT) < 0)
    {
      return -1;
    }

  esp_gpiowrite(pin, level ? true : false);
  return 0;
}

int board_touch_state(FAR struct board_touch_s *out)
{
  int x = g_x;
  int y = g_y;

  if (out == NULL)
    {
      return -EINVAL;
    }

  /* P4X-MAP: apply the selected transform. */

  {
    int nx = x;
    int ny = y;
    int m = g_map;

    if (m & 4)                    /* swap axes */
      {
        int t = nx; nx = ny; ny = t;
      }

    if (m & 1) nx = 1023 - nx;    /* mirror x */
    if (m & 2) ny = 599 - ny;     /* mirror y */

    x = nx;
    y = ny;
  }

  /* P4X-FILTER: ignore obviously invalid samples (outside the panel). */

  if (x < 0 || x > 1023 || y < 0 || y > 599)
    {
      out->pressed  = 0;
      out->x        = -1;
      out->y        = -1;
      out->reports  = g_reports;
      out->releases = g_release_count;
      return OK;
    }

  out->pressed   = g_pressed;
  out->x         = x;
  out->y         = y;
  out->reports   = g_reports;
  out->releases  = g_release_count;
  return OK;
}

#endif /* CONFIG_INPUT_GT9XX */

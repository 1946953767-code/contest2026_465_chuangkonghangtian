/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_touch.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_TOUCH_H
#define __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_TOUCH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct board_touch_s
{
  int           pressed;   /* Non-zero while a contact is reported */
  int           x;         /* Latest X coordinate */
  int           y;         /* Latest Y coordinate */
  unsigned long reports;   /* Total contact reports since boot */
  unsigned long releases;  /* Total release edges since boot */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_touch_initialize
 *
 * Description:
 *   Bring up I2C0 (SDA=GPIO7, SCL=GPIO8), detect the GT911 on the LCD module
 *   and start the polling task that publishes samples.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_touch_initialize(void);

/* P4X-BL: panel backlight control; returns the level read back from GPIO26. */

int board_backlight(int on);

/* P4X-BLREG: log the GPIO26 / GPIO27 output latch, output-enable and pad. */

void board_backlight_dump(const char *tag);

/* P4X-BLRST: drive the panel reset low, then release it. */

void board_panel_reset_pulse(void);

/****************************************************************************
 * Name: board_touch_state
 *
 * Description:
 *   Copy the latest touch sample.  Cheap and non-blocking, so the UI can call
 *   it from its render loop.
 *
 * Returned Value:
 *   Zero (OK) on success.
 *
 ****************************************************************************/

int board_touch_state(FAR struct board_touch_s *out);

/****************************************************************************
 * Name: board_touch_no_swipe
 *
 * Description:
 *   Tell the touch task that a horizontal drag starting inside this rectangle
 *   (mapped panel coordinates) is not a page swipe - used for on-screen
 *   buttons so a tap does not also turn the page.
 *
 ****************************************************************************/

void board_touch_no_swipe(int x0, int y0, int x1, int y1);

/****************************************************************************
 * Name: board_touch_flip
 *
 * Description:
 *   Return a swipe event recognised by the touch task (+1 next page, -1
 *   previous page) and clear it.  Returning the event from the sampler keeps
 *   page changes responsive: the UI applies it on its next frame instead of
 *   waiting for a lift edge.
 *
 ****************************************************************************/

int board_touch_flip(FAR int *dir);

/****************************************************************************
 * Name: board_touch_flip_tick / board_touch_flip_count
 *
 * Description:
 *   Tick at which the newest swipe was recognised, and the total number of
 *   swipes seen since boot (for latency profiling).
 *
 ****************************************************************************/

unsigned long board_touch_flip_tick(void);
unsigned long board_touch_flip_count(void);

/****************************************************************************
 * Name: board_fb_backbuffer
 *
 * Description:
 *   Return an off-screen framebuffer-sized buffer (allocated on first use from
 *   the PSRAM heap) so applications can compose a frame and publish it in one
 *   pass instead of being caught mid-update by the DSI scan-out.
 *
 ****************************************************************************/

FAR void *board_fb_backbuffer(size_t bytes);

/****************************************************************************
 * Name: board_fb_flush
 *
 * Description:
 *   Write back a region of the (cached) framebuffer so the DSI DMA sees it.
 *
 ****************************************************************************/

int board_fb_flush(FAR void *addr, size_t len);

/****************************************************************************
 * Name: board_touch_gt911_write
 *
 * Description:
 *   Write the GT911 module-switch byte (0x804D: bit0 mirror-x, bit1 mirror-y,
 *   bit2 swap) and optionally latch it by writing 0x8100 = 1.
 *
 ****************************************************************************/

int board_touch_gt911_write(uint8_t modsw, int commit);

int board_gpio_drive(int pin, int level);

#endif /* __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_TOUCH_H */

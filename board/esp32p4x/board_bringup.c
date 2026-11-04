/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_bringup.c
 *
 * Minimal board bring-up for the Espressif ESP32-P4X-Function-EV-Board on
 * openvela.  Scope of this first milestone: USB Serial/JTAG console, procfs
 * and the GPIO subsystem only.  No LCD, camera, audio, SD or Ethernet.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include "espressif/esp_gpio.h"
#include <nuttx/kthread.h>

#include <nuttx/video/fb.h>
#include <sys/mount.h>

#include <nuttx/fs/fs.h>

#include "board_pins.h"

#ifdef CONFIG_ESP32P4X_MIPI_DSI
#  include "esp32p4x_display.h"
#endif

#include "board_touch.h"

#if 1
#endif

#ifdef CONFIG_DEV_GPIO
int esp_gpio_init(void);
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_board_initialize
 *
 * Description:
 *   Called from the ESP32-P4 common start path (esp_start_p4.c) before the
 *   scheduler is fully up.  Keep this free of anything that needs tasks:
 *   the USB Serial/JTAG console is already initialised by the common layer.
 *
 ****************************************************************************/

/* P4X-SPLIT: the backlight is the only visible channel on a board whose panel
 * may stay dark, so the bring-up blinks it to report how far it got:
 *   1 blink  after the display is up
 *   2 blinks after the UI task was created
 */

static void p4x_bringup_blink(int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, false);
      usleep(220000);
      esp_gpiowrite(ESP32P4X_LCD_BACKLIGHT_GPIO, true);
      usleep(280000);
    }
}

void esp_board_initialize(void)
{
}

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Called from boardctl()/init once the OS services are available.
 *   This is where procfs and the board devices are mounted/registered.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{

  int ret = OK;

  /* P4X-BOOTFIX: making the console non-blocking here was tried and reverted -
   * it did not fix the cold boot and it broke NSH's readline.
   */


  UNUSED(arg);

#ifdef CONFIG_FS_PROCFS
#  ifdef CONFIG_NSH_PROC_MOUNTPOINT
#    define P4X_PROCFS_MOUNTPOINT CONFIG_NSH_PROC_MOUNTPOINT
#  else
#    define P4X_PROCFS_MOUNTPOINT "/proc"
#  endif

  ret = nx_mount(NULL, P4X_PROCFS_MOUNTPOINT, "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to mount procfs at %s: %d\n",
             P4X_PROCFS_MOUNTPOINT, ret);
      return ret;
    }
#endif

#ifdef CONFIG_DEV_GPIO
  ret = esp_gpio_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to initialise GPIO: %d\n", ret);
      return ret;
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   The panel is brought up here rather than in board_app_initialize():
 *   board_app_initialize() still runs inside nx_start(), and the EK79007
 *   power-on sequence needs real millisecond delays, which cannot complete
 *   until the scheduler tick is running.  board_late_initialize() runs from
 *   nx_bringup() after the OS services are available, so the delays and the
 *   framebuffer registration both work.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
/****************************************************************************
 * P4X-AUTOSTART: app entry points started with the system.
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_P4X_GS
/* The NuttX build renames the app's main() to <progname>_main for builtins. */

extern int p4x_gs_main(int argc, FAR char *argv[]);
#endif

#ifdef CONFIG_EXAMPLES_P4X_TOUCHPLOT
extern int p4x_touchplot_main(int argc, FAR char *argv[]);
#endif

#ifdef CONFIG_EXAMPLES_P4X_LORA
extern int p4x_lora_main(int argc, FAR char *argv[]);
#endif

#ifdef CONFIG_EXAMPLES_P4X_LOOP
extern int p4x_loop_main(int argc, FAR char *argv[]);
#endif

void board_late_initialize(void)
{
#ifdef CONFIG_BOARD_AUDIO
  /* P4X-AUDIO: ES8311 codec -> /dev/audio/pcm0p + pcm0c. */

  {
    extern int board_audio_initialize(void);
    int aret = board_audio_initialize();

    if (aret < 0)
      {
        syslog(LOG_WARNING, "WARNING: audio init failed: %d\n", aret);
      }
  }
#endif
  /* Panel bring-up is intentionally NOT part of the boot path yet.
   *
   * board_display_initialize() still wedges the CPU partway through the panel
   * power-on sequence on this port, and a wedged boot costs more than a dark
   * screen: the shell never appears.  openvela now boots to NSH with the
   * console quiet, and the panel work continues separately.
   *
   * When resuming, the two things already established are:
   *   1. VDD_MIPI_DPHY must be powered from board code before the DSI host is
   *      initialised (on-chip LDO channel 3 at 2500 mV), otherwise the PHY PLL
   *      never locks and the host init times out;
   *   2. the panel path that is known to light this display is the upstream one
   *      (esp_mipi_dsi_initialize() / up_fbinitialize()), not the
   *      esp32p4_mipi_dsi_* API currently used here.
   */

#ifdef CONFIG_ESP32P4X_MIPI_DSI
  int ret;

  /* Official NuttX entry point for this board: powers VDD_MIPI_DPHY through
   * on-chip LDO channel 3 and brings up the MIPI-DSI host.
   */

  ret = esp32p4x_display_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp32p4x_display_initialize failed: %d\n", ret);
      return;
    }

#ifdef CONFIG_VIDEO_FB
  /* The generic video layer calls up_fbinitialize(), which the official board
   * driver provides: it allocates the framebuffer (in PSRAM), programs the DPI
   * timing, runs the EK79007 power-on sequence, binds the framebuffer to the
   * DSI host, starts video and switches the backlight on.
   */

  /* GT911 touch panel on the LCD module (I2C0, SDA=GPIO7, SCL=GPIO8).
   * Registered as /dev/input0, which the ground station UI polls for swipes.
   */

  ret = board_touch_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: touch unavailable: %d\n", ret);
    }

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: fb_register(0,0) returned %d\n", ret);
    }

  p4x_bringup_blink(1);             /* P4X-SPLIT: display is up */
#endif
#endif

#ifdef CONFIG_EXAMPLES_P4X_GS
  /* P4X-AUTOSTART: the panel shows the ground station as soon as the system is
   * up; the UI brings up /dev/fb0 itself and advances pages every 8 s.
   */

  {
    FAR char *p4x_gs_argv[2];

    p4x_gs_argv[0] = (FAR char *)"p4x_gs";
    p4x_gs_argv[1] = NULL;

    ret = 0 && nxtask_create("p4x_gs", CONFIG_EXAMPLES_P4X_GS_PRIORITY,
                        NULL, CONFIG_EXAMPLES_P4X_GS_STACKSIZE,
                        p4x_gs_main, p4x_gs_argv, NULL);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: failed to start p4x_gs: %d\n", ret);
      }
    else
      {
        syslog(LOG_INFO, "ESP32-P4X: ground station UI started (pid %d)\n",
               ret);
      }
  }
#endif

#ifdef CONFIG_EXAMPLES_P4X_LOOP
  /* P4X-LOOP-AUTOSTART: verify UART1 itself before blaming the module. */

  {
    FAR char *loop_argv[2];

    loop_argv[0] = (FAR char *)"p4x_loop";
    loop_argv[1] = NULL;

    /* P4X-NOLOOPAUTO: it holds /dev/ttyS1, which starves the ground
     * station receive task.  Run "p4x_loop" by hand when the jumper is
     * fitted.
     */

    ret = 0 && nxtask_create("p4x_loop", 105, NULL, 4096,
                        p4x_loop_main, loop_argv, NULL);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: p4x_loop start failed: %d\n", ret);
      }
  }
#endif

#if 0   /* P4X-NOLORAAUTO: run p4x_lora by hand instead */
  /* /* P4X-NOLORAAUTO: the probe is left out of the boot path on purpose - it
  /*  * prints several lines per second and the panel was only stable on builds
  /*  * with a quiet console.  Run it by hand with the p4x_lora command.
  /*  */
  /* /* P4X-LORA-AUTOSTART: probe the A28 module and echo what arrives for 20 s,
  /*  * then leave the port open so the UI can report the link.
  /*  */
  /* {
  /*   FAR char *lora_argv[4];
  /*   lora_argv[0] = (FAR char *)"p4x_lora";
  /*   lora_argv[1] = (FAR char *)"-t";
  /*   lora_argv[2] = (FAR char *)"5";   /* P4X-QUIET: was 20 s */
  /*   lora_argv[3] = NULL;
  /*   /* P4X-NICEPRIO: below the UI (100) - at 110 it starved the render loop
  /*    * and the panel kept showing the boot frame.
  /*    */
  /*   ret = nxtask_create("p4x_lora", 120, NULL, 4096,
  /*                       p4x_lora_main, lora_argv, NULL);
  /*   if (ret < 0)
  /*     {
  /*       syslog(LOG_ERR, "ERROR: p4x_lora start failed: %d\n", ret);
  /*     }
  /*   else
  /*     {
  /*       syslog(LOG_INFO, "ESP32-P4X: LoRa probe task started (pid %d)\n", ret);
  /*     }
  /* }
#endif


#ifdef CONFIG_EXAMPLES_P4X_TOUCHPLOT
  /* P4X-PLOT-AUTOSTART: the on-screen touch test owns the panel in this build. */

  {
    FAR char *tp_argv[4];

    tp_argv[0] = (FAR char *)"p4x_touchplot";
    tp_argv[1] = (FAR char *)"3600";   /* seconds */
    tp_argv[2] = (FAR char *)"3";      /* touch map: 3 = mirror x + y */
    tp_argv[3] = NULL;

    ret = 0 && nxtask_create("touchplot", 100, NULL, 4096,
                        p4x_touchplot_main, tp_argv, NULL);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: p4x_touchplot start failed: %d\n", ret);
      }
    else
      {
        syslog(LOG_INFO, "ESP32-P4X: touch plot test started (pid %d)\n", ret);
      }
  }
#endif

#ifdef CONFIG_EXAMPLES_P4X_GS
  /* P4X-UI-GATE: the ground station UI starts unless P4X_NOUI=1 is exported,
   * which keeps the panel free for test programs.
   */

  if (getenv("P4X_NOUI") == NULL)
    {
      FAR char *p4x_gs_argv[2];

      p4x_gs_argv[0] = (FAR char *)"p4x_gs";
      p4x_gs_argv[1] = NULL;

      ret = nxtask_create("p4x_gs", CONFIG_EXAMPLES_P4X_GS_PRIORITY,
                          NULL, CONFIG_EXAMPLES_P4X_GS_STACKSIZE,
                          p4x_gs_main, p4x_gs_argv, NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: failed to start p4x_gs: %d\n", ret);
        }
      else
        {
          syslog(LOG_INFO, "ESP32-P4X: ground station UI started (pid %d)\n",
                 ret);

          p4x_bringup_blink(2);     /* P4X-SPLIT: UI task created */
        }
    }
#endif
}
#endif

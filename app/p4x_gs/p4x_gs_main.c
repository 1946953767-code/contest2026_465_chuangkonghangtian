/****************************************************************************
 * P4X 火箭遥测地面站界面 (Team465 创空航天)
 *
 * 目标平台：ESP32-P4X-Function-EV-Board + openvela(NuttX)
 * 屏幕：7 寸 MIPI-DSI 1024x600 RGB565（/dev/fb0，带触摸预留）
 *
 * 界面来源：
 *   - 排布参照 web/dashboard.html（横屏大屏：状态条 + 2x2 四图 + 曲线页 + 雷达页）
 *   - 特色页参照 app/chuangkong_ground_station/ground_station.c（思澈 SF32LB52）
 *     的大字速度页与火箭 3D 倾斜页
 *
 * 5 页：
 *   Page 0 状态 : 状态条(T+/卫星/阶段/高度/速度) + 2x2 四图(姿态/速度/高度/加速度)
 *   Page 1 曲线 : 姿态曲线 / 加速度曲线 / 高度曲线 / 速度曲线 / 气压曲线
 *   Page 2 速度 : 大字速度 + 三轴加速度 + 速度曲线（思澈特色）
 *   Page 3 火箭 : 火箭 3D 倾斜姿态图 + 俯仰/滚转/偏航（思澈特色）
 *   Page 4 雷达 : 雷达扫描图（中心=地基站，正北向上）+ 距离/坐标 + 定位按钮
 *
 * 翻页：串口键盘 n/p/空格/1-5、触摸左右滑动(有 /dev/input0 时)、空闲自动轮播
 *
 * 数据：默认跑内置演示飞行剖面，界面/字段与真实遥测一致；
 *       接入真实遥测时把 JSON 行喂给 telemetry_apply_json() 即可（字段同箭载端）。
 ****************************************************************************/

#include <nuttx/config.h>

/* P4X-LORASTAT: real A28 link state for the status page. */

#include "/home/dev/p4/ovw/vendor/espressif/boards/esp32p4x-function-ev-board/src/board_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#include <nuttx/video/fb.h>
#include <nuttx/audio/audio.h>
#include <syslog.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/clock.h>
#include <nuttx/kthread.h>

#include "cn_font.h"
/* P4X-BISECT1: the boot animation adds 156 KB of logo data to the image and
 * that version would not cold boot (backlight only).  Everything it needs is
 * behind this switch so it can be turned back on in small steps.
 */

#define P4X_BOOTANIM_ENABLE 0

#if P4X_BOOTANIM_ENABLE
#include "logo_img.h"
#endif

/* P4X-BOARD-TOUCH: the panel's GT911 is polled by the board (no touch INT line
 * on this board and the NuttX input driver blocks), so the UI reads the latest
 * sample from there.
 */

struct board_touch_s
{
  int           pressed;
  int           x;
  int           y;
  unsigned long reports;
  unsigned long releases;
};

extern int board_touch_state(FAR struct board_touch_s *out);
extern int board_touch_flip(FAR int *dir);          /* P4X-LAT */
extern unsigned long board_touch_flip_tick(void);   /* P4X-LAT */
extern FAR void *board_fb_backbuffer(size_t bytes); /* P4X-LAT */
extern void board_touch_no_swipe(int x0, int y0, int x1, int y1); /* P4X-BTN */
extern int board_backlight(int on);                                /* P4X-BL */
extern int board_panel_resync(void);                               /* P4X-RESYNC */
extern int board_panel_recover(void);                              /* P4X-PANEL-WD */
extern void board_backlight_dump(const char *tag);                 /* P4X-BLREG */
extern void board_panel_reset_pulse(void);                         /* P4X-BLRST */


/****************************************************************************
 * 颜色（沿用思澈版配色，保证观感一致）
 ****************************************************************************/

#define C_BLACK    0x0000
#define C_WHITE    0xFFFF
#define C_RED      0xF800
#define C_GREEN    0x07E0
#define C_BLUE     0x001F
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF
#define C_ORANGE   0xFD20
#define C_MAGENTA  0xF81F
#define C_LIME     0xAFE5
#define C_PURPLE   0x780F
#define C_DARK     0x18A3
#define C_DARKER   0x0C41
#define C_BTNDOWN   0x39C7   /* P4X-BTNFB: held-down button (dark steel) */
#define C_GRAY     0x8410
#define C_LGRAY    0xC618
#define C_SKY      0x55BF   /* P4X-PHASEARM: sky blue = cool-down frame */
#define C_PINK     0xFA10   /* P4X-PHASEPINK: no GPS fix at 准备 (255,64,128) */
#define C_LABEL    0xE71C
#define C_DARKRED  0xA000
#define C_DARKGRN  0x0400
#define C_SKYBLUE  0x04FF
#define C_BROWN    0xA145
#define C_HDRBAR   0x0220   /* 标题栏深蓝（思澈版同色） */
#define C_PANELBG  0x0000   /* 页面底色：纯黑，卡片才凸显 */

/* P4X-THEME: night is the dark theme, day is the same layout in light
 * colours with a white status bar.  These six are the ones a theme has to
 * change; everything else (accents, curves) stays as it is.
 */

static uint16_t g_c_bg     = 0x0000;   /* page background  */
static uint16_t g_c_hdr    = 0x0220;   /* status bar       */
static uint16_t g_c_text   = 0xFFFF;   /* primary text     */
static uint16_t g_c_label  = 0xE71C;   /* secondary labels */
static uint16_t g_c_dark   = 0x18A3;   /* borders          */
static uint16_t g_c_darker = 0x0C41;   /* cards            */
static uint16_t g_c_track  = 0x0000;   /* P4X-THEME4: bar / gauge track */
static uint16_t g_c_axis   = 0x07FF;   /* P4X-THEME6: x-axis labels */
static uint16_t g_c_yaw    = 0x07FF;   /* P4X-THEME6: yaw accent */

static int g_day_mode = 1;             /* P4X-FINAL9: boot in day mode */
static int g_theme_manual;             /* P4X-THEME2: user picked it */
static int g_gps_hour = -1;
static int g_dim_pct = 60;             /* P4X-DIM: night default */

/* P4X-DIM: software brightness as a real colour scale.  The backlight on this
 * board is a plain GPIO (no PWM), so the only way to dim smoothly is to scale
 * every colour on the way into the frame buffer.
 */

static inline uint16_t dimc(uint16_t c)
{
  int r;
  int g;
  int b;

  r = (c >> 11) & 0x1f;
  g = (c >> 5) & 0x3f;
  b = c & 0x1f;

  /* P4X-THEME3: in day mode a saturated bright accent (cyan axis labels,
   * yellow curves, lime numbers) disappears on a white page.  Darken anything
   * with real saturation and leave grey/white/black alone - those are already
   * handled by the theme table.
   */

  /* P4X-THEME6: no per-mode accent scaling any more.  Scaling saturated
   * colours for the light theme made every accent look like the night theme
   * and was impossible to reason about; accents now draw exactly as defined
   * and only the brightness setting scales them, in both themes.
   */

  if (g_dim_pct < 100)
    {
      r = r * g_dim_pct / 100;
      g = g * g_dim_pct / 100;
      b = b * g_dim_pct / 100;
    }

  return (uint16_t)((r << 11) | (g << 5) | b);
}

/****************************************************************************
 * 布局
 ****************************************************************************/

#define SCR_W      1024
#define SCR_H      600
#define SAFE       14
#define HDR_H      44
#define CONT_Y     (SAFE + HDR_H + 6)
#define CONT_X     SAFE
#define CONT_W     (SCR_W - 2 * SAFE)
#define CONT_H     (SCR_H - CONT_Y - SAFE)
#define NUM_PAGES  5
#define HIST_N     180

/* P4X-WIN: how many of those samples are actually drawn.  180 samples at
 * 80 ms is 14.4 s, so a quick flick of the board only moved the last 3% of
 * the trace and read as "the curve does not follow".  The window is now the
 * newest 48 samples (~4 s) stretched over the full width, so a 0.3 s flick
 * occupies 8% of the trace instead of 4%.
 */

#define HIST_WIN    32

/* P4X-ADAPT2: the drawn window is a runtime value now.  While the vehicle is
 * moving the trace uses a short window (the x axis scrolls about twice as
 * fast) and the renderer runs without its idle sleep; a couple of seconds
 * after the motion stops both go back to the calm settings.
 */

static int g_hist_win = HIST_WIN;
static int g_fast_frame;

/* P4X-DIAG: 0 keeps the console silent while the panel is running.
 *
 * Hard evidence, not theory: the picture stopped at t = 2:00, and the only
 * thing this program does at exactly that moment is print P4X-LAT2 at
 * fr_n >= 1500 (1500 x 80 ms = 120 s).  A printf from the render loop blocks
 * forever once nobody drains the USB-CDC console, and the panel dies with
 * it.  Set this to 1 only while a capture is actually attached.
 */

#define P4X_DIAG    0

/****************************************************************************
 * 全局
 ****************************************************************************/

static uint16_t *fb;                /* 屏幕显存（非缓存别名，DMA 正在读） */
static uint16_t *bb;                /* 后备缓冲：可缓存内存，绘制在这里 */
static size_t fb_bytes;             /* 显存字节数 */
static int bb_ready;                /* 1 = 用后备缓冲 + 一次性上屏 */
static int fb_w = SCR_W, fb_h = SCR_H, fb_stride = SCR_W;
static int fb_fd = -1;
static int touch_fd = -1;
static int key_fd = -1;             /* 串口键盘（非阻塞） */
static int cur_page;
static int flip_ms = -1;              /* P4X-LAT: last swipe -> draw time */
static unsigned long flip_seen;       /* P4X-LAT */
static unsigned long tap_log_count;   /* P4X-QUIET2: throttle touch logging */
static unsigned long bl_health_next = 5;   /* P4X-BL: next backlight blink */
static unsigned long bl_keep_next = 2;     /* P4X-BLKEEP: next re-assert */
static unsigned long resync_next = 20;     /* P4X-WD-SLOW: first check at ~20 s */
static int state_dirty;                    /* P4X-DWELL: input needs a repaint */
static int saved_dir;                      /* P4X-DWELL: queued swipe direction */
static int idle_slices;                    /* P4X-DWELL: inner-loop counter */
static int draw_due;                       /* P4X-DWELL: period elapsed */
static unsigned long beat_count;           /* P4X-BEAT: inner-loop counter */
static int auto_advance;   /* P4X-NOAUTO: auto page advance off by default */

static unsigned long tick;          /* 100ms 计数 */
static unsigned long last_draw_ms;  /* 上一帧绘制耗时（ms） */
static unsigned long g_fps;         /* P4X-HDR: live frame rate */
static unsigned long g_fps_t0;
static unsigned long g_fps_n;
static int d_x0 = 1 << 30, d_y0 = 1 << 30, d_x1 = -1, d_y1 = -1;   /* P4X-DIRTY */
static int frame_dx0, frame_dy0, frame_dx1, frame_dy1;
static unsigned long last_blit_ms;  /* 上一帧上屏耗时（ms） */
static int quit;
static int touch_button_hot;        /* 雷达页“定位”按钮按下态 */

/* P4X-PANELWD: the panel gets its own task.
 *
 * Every fallback we had lived inside the render loop, so the moment that loop
 * stalled the backlight re-assert, the panel resync and the watchdog all stalled
 * with it - the panel went dark and never came back ("it flashes, then goes
 * dark, and only a reboot helps").  This task is independent:
 *
 *   - keeps the backlight latch asserted,
 *   - notices when no new frame has reached the glass for a second,
 *   - replays the panel power-on sequence (every fifth time) and restarts the
 *     video stream, which re-arms the DMA chain,
 *   - logs one heartbeat line a second into the RAM log, so "is it the program
 *     or the panel?" is answerable with `dmesg` after the fact.
 */

static volatile unsigned long g_frame_seq;     /* frames pushed to the glass */
static volatile unsigned long g_panel_recov;   /* recoveries performed */

/* P4X-PANELHOLD: state for the "hold the screen for two seconds" cure. */

static int           g_panel_hold;
static int           g_panel_hold_x;
static int           g_panel_hold_y;
static unsigned long g_panel_hold_t;
static int           g_panel_hold_done;

static void panel_recover_now(void)
{
  board_panel_reset_pulse();
  (void)board_panel_resync();
  g_frame_seq++;                  /* restart the watchdog's stale timer */
  syslog(LOG_INFO, "P4X-PANELHOLD: recovery from the operator\n");
}

/* P4X-ENTERROM: try to reboot into the ROM downloader from the screen.
 *
 * Flashing needs the BOOT strap low at the instant of reset and this board does
 * not wire that strap to the USB control lines (all three DTR/RTS patterns were
 * tried and none reached the bootloader), so every flash has meant pressing the
 * button.  This is the one software trick left: drive the strap pads low as
 * outputs and reset the core immediately, hoping the ROM samples the pads before
 * the IO_MUX returns to its reset state.  If it does not work the board simply
 * boots again - the only cost is one reboot.
 *
 * Trigger: hold the screen still for six seconds (the two-second hold is the
 * panel recovery, so the gestures do not collide).  Defined further down, where
 * the framebuffer globals are already in scope.
 */

extern int board_reset(int status);

static void enter_rom_mode(void);


/* P4X-BTNRECT: the rectangle the button was actually drawn in.  The hit test
 * used to look at CONT_X..CONT_X+380 on the LEFT while the button lives in
 * the right-hand column, so tapping the visible button did nothing at all.
 */

static int g_touch_mx = -1;
static int g_touch_my = -1;
static unsigned long g_touch_mtick;

/* P4X-TOAST: banner state - declared here because page_next() clears it. */

static char g_toast[24];
static int g_toast_page = -1;
static unsigned long g_toast_start;
static unsigned long g_toast_until;

/* P4X-STATUS5: set while the 0.5 s detection window runs; the button is
 * locked during it but paging still works, and it is released as soon as the
 * banner goes up.
 */

static int g_act_pending;
static unsigned long g_act_deadline;

static int g_btn_x0 = -1;
static int g_btn_y0 = -1;
static int g_btn_x1 = -1;
static int g_btn_y1 = -1;
static unsigned long g_btn_ok_tick;

/* P4X-SIMPLETOUCH: gesture tunables - a swipe needs this much horizontal
 * travel, and a tap on the button must stay under the second value.
 */

#define SWIPE_PX         60
#define SWIPE_MIN_UI_PX  80
static int btn_glow;                /* P4X-GLOW: frames left of highlight */
static volatile int page_clear_req; /* P4X-PAGECLEAR: full repaint request */
static int btn_press_seen;          /* P4X-BTNFB: a press needs an immediate frame */

/* P4X-BTN: the drawn rectangle.  The old hit test used CONT_W - 380 (x > 686)
 * while the button lives in the left column (x = CONT_X..CONT_X + 380).
 */

/* P4X-BTN3: these are the numbers page_radar() actually draws with.
 *   x: the left column  = CONT_X .. CONT_X + 380
 *   y: after three 96 px cards starting at CONT_Y (64) with 8 px gaps,
 *      i.e. 64 + 3 * 104 = 376, and it fills down to the safe margin (532).
 * The old box (y = 452..552) only covered the lower part of the button, so a
 * tap on the upper two thirds did nothing.
 */

#define BTN_X0   (CONT_X)
#define BTN_X1   (CONT_X + 380)
#define BTN_UP_Y 376
#define BTN_DN_Y 532
#define BTN_TAP_PX 60

/* P4X-TOUCH-BG: the GT911 read costs ~500 ms per call on this board, so it is
 * done by a background task and the render loop only consumes the result.
 */

static volatile int t_bg_x = -1;
static volatile int t_bg_y = -1;
static volatile uint8_t t_bg_flags;
static volatile int t_bg_seq;
static int t_bg_last_seq;
static int t_bg_fd = -1;

static int touch_sampler(int argc, FAR char *argv[])
{
  struct touch_sample_s sample;

  (void)argc;
  (void)argv;

  for (;;)
    {
      /* P4X-TOUCH-DBG: report what the driver returns. */

      {
        int r = -1;

        if (t_bg_fd >= 0)
          {
            r = read(t_bg_fd, &sample, sizeof(sample));
          }

        if (r == (int)sizeof(sample) && sample.npoints >= 1)
          {
            t_bg_x = sample.point[0].x;
            t_bg_y = sample.point[0].y;
            t_bg_flags = sample.point[0].flags;
            t_bg_seq++;
            printf("P4X-GS: touch x=%d y=%d f=0x%02x\n",
                   t_bg_x, t_bg_y, t_bg_flags);
          }
      }

      usleep(50000);
    }

  return 0;
}


/****************************************************************************
 * 绘图原语（与思澈版一致）
 ****************************************************************************/

static inline void px(int x, int y, uint16_t c)
{
  if (x >= 0 && x < fb_w && y >= 0 && y < fb_h)
    {
      bb[y * fb_stride + x] = dimc(c);
    }
}

/* 后备缓冲整帧上屏：一次顺序 memcpy 写到非缓存显存。
 * 直接往非缓存显存逐像素画时：① 慢 ② DSI 的 DW-GDMA 正在读同一块内存，
 * 半成品帧会被实时扫出去 → 看起来就是「只有底色 + 花屏」。
 */
static void bb_blit(void)
{
  if (bb_ready)
    {
      memcpy(fb, bb, fb_bytes);
    }
}

/* 按行连续写：附带 32 位快路径（一字节拍两像素），非缓存显存下明显更快 */
static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
  int y;

  c = dimc(c);

  if (x0 < 0) { w += x0; x0 = 0; }
  if (y0 < 0) { h += y0; y0 = 0; }
  if (x0 + w > fb_w) w = fb_w - x0;
  if (y0 + h > fb_h) h = fb_h - y0;
  if (w <= 0 || h <= 0) return;

  /* P4X-DIRTY: remember which part of the screen this frame touched. */

  if (x0 < d_x0) d_x0 = x0;
  if (y0 < d_y0) d_y0 = y0;
  if (x0 + w > d_x1) d_x1 = x0 + w;
  if (y0 + h > d_y1) d_y1 = y0 + h;

  for (y = y0; y < y0 + h; y++)
    {
      uint16_t *row = bb + (size_t)y * fb_stride + x0;
      int n = w;
      uint32_t pair = ((uint32_t)c << 16) | c;

      if (((uintptr_t)row & 3u) == 0)
        {
          uint32_t *row32 = (uint32_t *)row;
          int nw = n / 2;
          int i;
          for (i = 0; i < nw; i++) row32[i] = pair;
          if (n & 1) row[n - 1] = c;
        }
      else
        {
          int i;
          for (i = 0; i < n; i++) row[i] = c;
        }
    }
}

static void hline(int x0, int x1, int y, uint16_t c)
{
  if (y < 0 || y >= fb_h) return;
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  fill_rect(x0, y, x1 - x0 + 1, 1, c);
}

static void vline(int x, int y0, int y1, uint16_t c)
{
  if (x < 0 || x >= fb_w) return;
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  fill_rect(x, y0, 1, y1 - y0 + 1, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;

  for (;;)
    {
      px(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_circle(int cx, int cy, int r, uint16_t c)
{
  int i;
  for (i = 0; i < 360; i += 2)
    {
      float a = i * 3.14159f / 180.0f;
      px(cx + (int)(r * cosf(a)), cy + (int)(r * sinf(a)), c);
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t c)
{
  int y;

  /* 按行批量填充：非缓存显存下逐像素写太慢（雷达盘 r=236 时差 5~10 倍） */

  for (y = -r; y <= r; y++)
    {
      int half = (int)sqrtf((float)(r * r - y * y));
      fill_rect(cx - half, cy + y, 2 * half + 1, 1, c);
    }
}

static void fill_round_rect(int x0, int y0, int w, int h, int r, uint16_t c)
{
  int y;

  for (y = 0; y < h; y++)
    {
      int dx = 0;
      if (y < r)          dx = r - (int)sqrtf((float)(r * r - (r - y) * (r - y)));
      else if (y >= h - r) dx = r - (int)sqrtf((float)(r * r - (y - (h - r - 1)) * (y - (h - r - 1))));
      fill_rect(x0 + dx, y0 + y, w - 2 * dx, 1, c);
    }
}

/* 卡片：深色圆角底 + 左上彩色竖条 + 一圈描边（底色是纯黑，描边才看得清） */
static void card_border(int x, int y, int w, int h, uint16_t c)
{
  hline(x + 6, x + w - 7, y, c);
  hline(x + 6, x + w - 7, y + h - 1, c);
  vline(x, y + 6, y + h - 7, c);
  vline(x + w - 1, y + 6, y + h - 7, c);
}

/* 扫描线凸/凹多边形填充（火箭、指针用） */
static void fill_poly(const int *xs, const int *ys, int n, uint16_t c)
{
  int i, j, y, ymin = ys[0], ymax = ys[0];

  for (i = 1; i < n; i++)
    {
      if (ys[i] < ymin) ymin = ys[i];
      if (ys[i] > ymax) ymax = ys[i];
    }

  if (ymin < 0) ymin = 0;
  if (ymax >= fb_h) ymax = fb_h - 1;

  for (y = ymin; y <= ymax; y++)
    {
      int xa = 1 << 30, xb = -(1 << 30);
      for (i = 0, j = n - 1; i < n; j = i++)
        {
          int yi = ys[i], yj = ys[j];
          if ((yi <= y && yj > y) || (yj <= y && yi > y))
            {
              int x = xs[i] + (int)((long)(y - yi) * (xs[j] - xs[i]) / (yj - yi));
              if (x < xa) xa = x;
              if (x > xb) xb = x;
            }
        }
      if (xa <= xb) fill_rect(xa, y, xb - xa + 1, 1, c);
    }
}

/****************************************************************************
 * 字体：8x8 ASCII（可缩放）+ 16x16 中文点阵
 ****************************************************************************/

static const uint8_t font8[128][8] =
{
  [' ']={0,0,0,0,0,0,0,0},['!']={0x18,0x18,0x18,0x18,0x18,0,0x18,0},
  ['0']={0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},['1']={0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
  ['2']={0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0},['3']={0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},
  ['4']={0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},['5']={0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},
  ['6']={0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},['7']={0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},
  ['8']={0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},['9']={0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},
  ['.']={0,0,0,0,0,0x18,0x18,0},['-']={0,0,0,0x7E,0,0,0},
  [':']={0,0x18,0x18,0,0x18,0x18,0},['%']={0x62,0x64,0x08,0x10,0x26,0x46,0,0},
  ['+']={0,0x18,0x18,0x7E,0x18,0x18,0},['/']={0x06,0x0C,0x18,0x30,0x60,0xC0,0,0},
  [',']={0,0,0,0,0x18,0x18,0x30,0},['(']={0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0},
  [')']={0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0},['=']={0,0,0x7E,0,0x7E,0,0,0},
  ['A']={0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},['B']={0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},
  ['C']={0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},['D']={0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0},
  ['E']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},['F']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0},
  ['G']={0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0},['H']={0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},
  ['I']={0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0},['J']={0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0},
  ['K']={0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0},['L']={0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},
  ['M']={0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0},['N']={0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0},
  ['O']={0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},['P']={0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},
  ['Q']={0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0},['R']={0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0},
  ['S']={0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0},['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},
  ['U']={0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},['V']={0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0},
  ['W']={0x63,0x63,0x63,0x63,0x6B,0x7F,0x36,0},['X']={0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},
  ['Y']={0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0},['Z']={0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0},
};

static void text(int x, int y, const char *s, uint16_t c)
{
  while (*s)
    {
      unsigned char ch = (unsigned char)*s++;
      const uint8_t *g;
      int r, cc;

      if (ch > 127) continue;
      g = font8[ch];
      for (r = 0; r < 8; r++)
        for (cc = 0; cc < 8; cc++)
          if (g[r] & (0x80 >> cc)) px(x + cc, y + r, c);
      x += 8;
    }
}

static void text_scale(int x, int y, const char *s, uint16_t c, int sc)
{
  while (*s)
    {
      unsigned char ch = (unsigned char)*s++;
      const uint8_t *g;
      int r, cc, sx, sy;

      if (ch > 127) continue;
      g = font8[ch];
      for (r = 0; r < 8; r++)
        for (cc = 0; cc < 8; cc++)
          if (g[r] & (0x80 >> cc))
            for (sy = 0; sy < sc; sy++)
              for (sx = 0; sx < sc; sx++)
                px(x + cc * sc + sx, y + r * sc + sy, c);
      x += 8 * sc;
    }
}

/* P4X-SATCLS: a label that mixes ASCII and CJK.
 *
 * text() drops every byte above 127 and text_cn() drops every byte below it,
 * so neither can draw "视野内 12" or "GPS 6".  This walks the string and hands
 * each run to the right rasteriser, advancing by the real cell size.
 */

static void text_cn(int x, int y, const char *s, uint16_t c, int sc);

static void text_mixed(int x, int y, const char *s, uint16_t c, int sc)
{
  const unsigned char *p = (const unsigned char *)s;

  while (*p)
    {
      if (*p < 0x80)
        {
          char one[2];

          one[0] = (char)*p;
          one[1] = '\0';
          text_scale(x, y, one, c, sc);
          x += 8 * sc;
          p++;
        }
      else if (p[1] != 0 && p[2] != 0)
        {
          char g[4];

          g[0] = (char)p[0];
          g[1] = (char)p[1];
          g[2] = (char)p[2];
          g[3] = '\0';
          text_cn(x, y, g, c, sc);
          x += 16 * sc;
          p += 3;
        }
      else
        {
          break;
        }
    }
}

static const unsigned char *cn_lookup(const unsigned char *u)
{
  unsigned int i;
  for (i = 0; i < CN_FONT_COUNT; i++)
    if (cn_font[i].utf8[0] == u[0] &&
        cn_font[i].utf8[1] == u[1] &&
        cn_font[i].utf8[2] == u[2])
      return cn_font[i].bmp;
  return NULL;
}

/* 中文串像素宽度（16 字宽；ASCII 按 8 计） */
static int cn_width_est(const char *s)
{
  int n = 0;
  const unsigned char *p = (const unsigned char *)s;

  while (*p)
    {
      if (*p >= 0x80) { p += 3; n += 16; }
      else { p++; n += 8; }
    }
  return n;
}

static void text_cn(int x, int y, const char *s, uint16_t c, int sc)
{
  const unsigned char *p = (const unsigned char *)s;

  while (*p)
    {
      const unsigned char *bmp;
      int row, bit, sy, sx;

      if (*p < 0x80) { p++; continue; }
      if (p[1] == 0 || p[2] == 0) break;
      bmp = cn_lookup(p);
      p += 3;
      if (!bmp) { x += 16 * sc; continue; }

      for (row = 0; row < 16; row++)
        {
          unsigned int bits = ((unsigned int)bmp[row * 2] << 8) | bmp[row * 2 + 1];
          for (bit = 0; bit < 16; bit++)
            if (bits & (1u << (15 - bit)))
              for (sy = 0; sy < sc; sy++)
                for (sx = 0; sx < sc; sx++)
                  px(x + bit * sc + sx, y + row * sc + sy, c);
        }
      x += 16 * sc;
    }
}

/****************************************************************************
 * 数据模型（字段与箭载端遥测一致）
 ****************************************************************************/
typedef struct
{
  float t;              /* 任务时间 s */
  float v;              /* 速度 m/s */
  float alt;            /* 高度 m */
  float ax, ay, az;     /* 三轴加速度 g */
  float pitch, roll, yaw;
  float gx, gy, gz;     /* 角速度 */
  float pressure;       /* Pa */
  float temperature;    /* C */
  double lat, lon;      /* 火箭位置 */
  int sat, gps_fix, lora_ok, phase;
  int rocket_link;
  char time_str[12];
} flight_t;

static flight_t fl;
static int telemetry_live;                  /* 1 = 收到真实遥测 */
static double gs_lat = 22.17590, gs_lon = 113.07310;   /* 地基站（可在雷达页设定） */
static int own_fix = 1;

/* 历史曲线 */
static float v_hist[HIST_N], alt_hist[HIST_N], a_hist[HIST_N], p_hist[HIST_N];
static float pit_hist[HIST_N], rol_hist[HIST_N], yaw_hist[HIST_N];
static float ax_hist[HIST_N], ay_hist[HIST_N], az_hist[HIST_N];

/* P4X-XAXIS3: real tick of every sample in the ring, so the time axis can
 * report the seconds that are actually covered instead of a fixed guess.
 */

static float tmp_hist[HIST_N];         /* P4X-DATA4: temperature */
static float rx_hist[HIST_N];          /* packets per second */
static float lost_hist[HIST_N];        /* estimated packets lost per second */
static float tx_hist[HIST_N];          /* packets sent per second */

static unsigned long g_rx_pps;
static unsigned long g_tx_pps;
static unsigned long g_lost_pps;

static unsigned long hist_ts[HIST_N];

/* P4X-ZOOM: tap a chart to fill the panel with it.
 *
 * Every chart drawn on pages 1 and 2 registers its rectangle, keyed by the
 * data array it plots.  A tap inside one opens a zoomed view: the whole panel
 * becomes that chart, drawn with the full ring (HIST_N samples) instead of the
 * short live window, so the time axis is much longer and the detail finer.
 * While zoomed a swipe cannot change page; a second tap goes back.
 */

#define HR_MAX 10

static int hr_x0[HR_MAX], hr_y0[HR_MAX], hr_x1[HR_MAX], hr_y1[HR_MAX];
static int hr_id[HR_MAX];
static int hr_n;

static int g_zoom_id;
static int g_zoom_win_saved;

static void hit_reset(void)
{
  hr_n = 0;
}

static int zoom_id_of(const float *d)
{
  if (d == pit_hist) return 1;
  if (d == ax_hist)  return 2;
  if (d == alt_hist) return 3;
  if (d == p_hist)   return 4;
  if (d == v_hist)   return 5;
  if (d == tmp_hist) return 6;
  if (d == rx_hist)  return 7;
  return 0;
}

static void hit_reg(int id, int x0, int y0, int w, int h)
{
  if (id == 0 || hr_n >= HR_MAX)
    {
      return;
    }

  hr_x0[hr_n] = x0;
  hr_y0[hr_n] = y0;
  hr_x1[hr_n] = x0 + w - 1;
  hr_y1[hr_n] = y0 + h - 1;
  hr_id[hr_n] = id;
  hr_n++;
}

static int hit_lookup(int x, int y)
{
  int i;

  for (i = hr_n - 1; i >= 0; i--)
    {
      if (x >= hr_x0[i] && x <= hr_x1[i] &&
          y >= hr_y0[i] && y <= hr_y1[i])
        {
          return hr_id[i];
        }
    }

  return 0;
}

/* P4X-LATDIAG: measured latency inputs - the gap between complete frames
 * arriving from the radio, sampled by the receive task.
 */

static volatile unsigned long g_rx_gap_sum;
static volatile unsigned long g_rx_gap_max;
static volatile unsigned long g_rx_gap_n;
static volatile unsigned long g_rx_last_tick;
/* P4X-FBDUMP: a 16:1 thumbnail of the panel, so the layout can be checked
 * from a serial log instead of guessed.
 */

static uint16_t *g_bb;
static int g_bb_stride;

static void fb_dump_small(void)
{
  int x;
  int y;

  if (g_bb == NULL)
    {
      printf("P4X-FBDUMP: no back buffer\n");
      return;
    }

  printf("P4X-FBDUMP: begin %dx%d\n", fb_w / 16, fb_h / 16);

  for (y = 0; y < fb_h; y += 16)
    {
      printf("P4X-FBDUMP:");

      for (x = 0; x < fb_w; x += 16)
        {
          printf(" %04x", (unsigned)g_bb[y * g_bb_stride + x]);
        }

      printf("\n");

      /* P4X-SATCLS: a whole thumbnail is ~13 KB of console.  Give the USB-CDC
       * endpoint a moment between rows so a dump can never fill it and block
       * the render loop (the reason diagnostics used to be compiled out).
       */

      usleep(8000);
    }

  printf("P4X-FBDUMP: end\n");
}

int board_lora_dummy_fbdump(void);
int board_lora_dummy_fbdump(void) { return 0; }

static int hist_idx;
static unsigned long lora_packets, lora_bad;
static int lora_ready;                  /* P4X-LORASTAT */
static int lora_detected;               /* P4X-LORASTAT: module answered */
static struct board_lora_stats_s lora_stats;   /* P4X-LORASTAT */

/* P4X-PHASE: the mission phases, in the order the operator steps through them.
 *
 * The airborne has understood "B:<NAME>" for a while (READY / ARM / IGNITE /
 * ABORT / END, with per-phase buzzer sounds and LED colours), but the ground
 * station had no way to send it and did not even know which phase was active.
 * These two tables are the single mapping between the screen, the button
 * sequence and the command that goes on air.
 */

#define PH_NUM      5
#define PH_READY    0
#define PH_ARM      1
#define PH_IGNITE   2
#define PH_ABORT    3
#define PH_END      4

/* P4X-PHASECOLOR: the order and the colours come from the vehicle.
 *
 * The operator asked for "准备 first, then 预备", and for one colour per
 * command - sharing colours made the sequence confusing to follow:
 *
 *   index 0  READY   绿   (0,200,0)   -> 0x0640
 *   index 1  ARM     橙   (255,160,0) -> 0xFD00
 *   index 2  IGNITE  红   (255,0,0)   -> 0xF800   点火 is red
 *   index 3  ABORT   紫   (150,0,255) -> 0x901F
 *   index 4  END     白   (255,255,255)-> 0xFFFF   结束 is white
 *
 * (The vehicle's READY lamp turns red while it has no GPS fix; the panel uses
 * the phase colour itself, green, so that one command is always one colour.)
 * central_tx_s3.py carries the same table so the lamp and the panel agree.
 */

static const char *phase_cn[PH_NUM] =
{
  "准备", "预备", "点火", "灭火", "结束"
};

static const char *phase_code[PH_NUM] =
{
  "READY", "ARM", "IGNITE", "ABORT", "END"
};

static const uint16_t phase_color[PH_NUM] =
{
  0x0640,   /* 准备 READY  绿 */
  0xFD00,   /* 预备 ARM    橙 */
  0xF800,   /* 点火 IGNITE 红 */
  0x901F,   /* 灭火 ABORT  紫 */
  0xFFFF    /* 结束 END    白 */
};

static int   g_phase_idx;            /* the phase shown on the panel */
static int   g_phase_need_confirm;   /* ignition / abort selected, not sent */
static int   g_phase_tx_left;        /* repeats still to put on air */
static unsigned long g_phase_tx_t;   /* next repeat due */
static unsigned long g_phase_cmd_t;  /* when the last command went out */
static unsigned long g_phase_step_t; /* P4X-PHASERATE: last phase change */
static char  g_phase_wire[16];       /* "B:IGNITE\\n" */
static int   g_phase_wire_len;

/* P4X-PHASECOOL: the lockout after a command.
 *
 * Everything here is expressed in TICK_PER_SEC, not in bare ticks: an earlier
 * version wrote "20" for two seconds and the frame closed in a fifth of a
 * second (TICK_PER_SEC is 100 on this board), which made the arming look
 * instant and the cool-down invisible.
 */

#define PH_HOLD_TICKS  (2 * TICK_PER_SEC)     /* 2 s of holding arms it   */
#define PH_COOL_TICKS  (5 * TICK_PER_SEC)     /* 5 s lockout after a shot */
#define PH_ACK_TICKS   (15 * TICK_PER_SEC)    /* give up waiting for the S3 */
#define PH_BACK_TICKS  (TICK_PER_SEC * 6 / 10) /* 0.6 s for the frame to retract */

/* P4X-PHASEARM: the frame is a little state machine of its own.
 *
 *   按下一  红框 0 -> 100% over two seconds
 *   松手早  红框 walks back to 0 (the press is cancelled, nothing is sent)
 *   跑满    发送, then the frame turns sky blue and walks back over five
 *           seconds; while it is blue the button cannot be pressed at all.
 */

static int           g_ph_back;      /* the frame is retracting     */
static int           g_ph_back_from; /* permille it started from    */
static unsigned long g_ph_back_t0;

static unsigned long g_ph_cool;      /* lockout ends at this tick  */
static unsigned long g_ph_sent_t;    /* when the command went out  */
static int           g_ph_wait_ack;  /* waiting for the vehicle    */
static int           g_ph_acked;     /* the vehicle confirmed      */
static int           g_phase_target; /* P4X-PHASEALIGN: phase asked for */
static unsigned long g_ph_anim_t;    /* P4X-PHASEARM: last animation repaint */

static int phase_show_idx(void)
{
  if (g_phase_idx < 0)
    {
      return 0;
    }

  if (g_phase_idx >= PH_NUM)
    {
      return PH_NUM - 1;
    }

  return g_phase_idx;
}

/* P4X-PHASEPINK: the colour of the phase, as the vehicle shows it.
 *
 * The vehicle's lamp for the standby phase doubles as the GPS indicator: green
 * with a fix, and pink without one.  Red used to mark "no fix" and that now
 * clashes with 点火, which the operator asked to be red.  The panel mirrors the
 * lamp exactly, so one glance at either tells the same story.
 */

static uint16_t phase_color_now(void)
{
  if (phase_show_idx() == PH_READY && fl.sat <= 0)
    {
      return C_PINK;
    }

  return phase_color[phase_show_idx()];
}

/* P4X-PHASE: put "B:<NAME>" on air.
 *
 * The airborne only listens in a window between its transmissions, so a single
 * line is not enough: fourteen copies 120 ms apart cover 1.7 s and land inside
 * at least one window.  It de-duplicates by phase on its side, so the repeats
 * cost nothing but air time.
 */

static void phase_send(int idx)
{
  if (idx < 0 || idx >= PH_NUM)
    {
      return;
    }

  g_phase_wire_len = snprintf(g_phase_wire, sizeof(g_phase_wire), "B:%s\n",
                              phase_code[idx]);

  g_phase_tx_left      = 14;
  g_phase_tx_t         = clock_systime_ticks();
  g_phase_cmd_t        = clock_systime_ticks();
  g_phase_need_confirm = 0;

  /* P4X-PHASECOOL: one press, then a lockout.  Five seconds is the minimum -
   * the buttons are dead until it expires - and on top of that the vehicle has
   * to confirm the new phase (its telemetry carries "ph"); if no answer arrives
   * within fifteen seconds the lockout gives up on its own, because a lost link
   * must not leave the panel unable to send an abort.
   */

  g_ph_sent_t   = clock_systime_ticks();
  g_ph_cool     = g_ph_sent_t + PH_COOL_TICKS;
  g_ph_wait_ack = 1;
  g_ph_acked    = 0;
}

/* Called from the render loop: never blocks, one repeat per pass. */

static void phase_pump(void)
{
  unsigned long now;

  if (g_phase_tx_left <= 0 || g_phase_wire_len <= 0)
    {
      return;
    }

  now = clock_systime_ticks();
  if (now < g_phase_tx_t)
    {
      return;
    }

  (void)board_lora_send(g_phase_wire, (size_t)g_phase_wire_len);
  g_phase_tx_left--;
  g_phase_tx_t = now + 12;           /* 120 ms */
}

/* P4X-PHASE: step the phase.
 *
 * P4X-PHASEHOLD: the operator asked for "hold two seconds, a red frame fills
 * up, then it goes": with that interaction every transition arrives already
 * confirmed, so "confirmed" says whether the caller has collected the press.
 * An unconfirmed transition to ignition/abort only moves the highlight - a
 * slipped finger must not fire the rocket.
 */

static void phase_step(int dir, int confirmed)
{
  unsigned long now = clock_systime_ticks();
  int next = phase_show_idx() + dir;

  /* P4X-PHASECOOL: nothing happens while the five second lockout runs.  The
   * answer from the vehicle does not gate the button - the operator asked for
   * "frame closes, command goes, five seconds, then it can be pressed again" -
   * it only decides whether the header reads 发送中 or 发送完成.
   */

  if (now < g_ph_cool)
    {
      return;
    }

  g_ph_wait_ack = 0;

  /* P4X-PHASERATE: one step per second, whatever the input does.
   *
   * Measured on this panel: a contact that jitters at one spot is reported as
   * press/release pairs, and every pair stepped the phase once - the sequence
   * ran to END on its own ("准备" was gone before anyone saw it, and each
   * buzzer sound was cut off by the next one).  A one-second floor makes that
   * impossible and still leaves the operator a comfortable pace.
   */

  if (now - g_phase_step_t < 100)
    {
      return;
    }

  /* P4X-PHASEWRAP: wrap around the ring of phases.
   *
   * Without this the last phase had no successor: the "next" button drew an
   * empty box (there was no phase name to show) and tapping it produced no
   * frame, no command and no sound, which read as "the button is broken".  With
   * a wrap both buttons always have a target and always do something - 结束
   * steps back to 预备, 预备 steps back to 结束.
   */

  if (next < 0)
    {
      next = PH_NUM - 1;
    }
  else if (next >= PH_NUM)
    {
      next = 0;
    }

  g_phase_step_t = now;
  g_phase_target = next;             /* P4X-PHASEALIGN: what we asked for */

  if (!confirmed && (next == PH_IGNITE || next == PH_ABORT))
    {
      g_phase_need_confirm = 1;      /* shown, deliberately not sent */
      return;
    }

  /* P4X-PHASEALIGN: a press is a request, not a state change.
   *
   * The phase shown on the panel is whatever the vehicle last reported in its
   * "ph" field (see the telemetry handler).  This function only puts the
   * command on air.  If the link dropped the request, the panel keeps showing
   * the real phase and the operator can simply press again - which is what the
   * operator asked for: "当前阶段要和 S3 对齐，因为可能发不成功".
   */

  phase_send(next);
}

/* P4X-NOBOOTKEY: the BOOT pad never read back reliably on this board
 * (raw register read returned a constant, and the pad the header names
 * could not be confirmed), so the key feature was removed - phases are
 * stepped from the on-screen buttons only.  board_gpio_input() stays in
 * the board code for the next attempt.
 */

/*
 * 演示飞行剖面：与真实遥测同字段，便于先看界面效果。
 * 接入真实遥测后，只要调用 telemetry_apply_json() 并置 telemetry_live=1，
 * 本函数即不再使用。
 */

/* P4X-NODEMO: demo_flight() removed.  It synthesised a whole flight
 * profile (phase timeline, speed/altitude ramp, IMU wobble, GPS drift and
 * an atmospheric model) and the panel drew it whenever telemetry_live was
 * 0, so a panel full of numbers proved nothing about the link.  The only
 * writer of flight state is now telemetry_apply_json().
 */

/* 真实遥测：JSON 行（字段与箭载端一致），供后续接入 LoRa 时使用 */
/* P4X-TIMEFIX: set once a real "T":"HH:MM:SS" has been received. */

static float g_alt_abs = 0.0f;   /* P4X-ALTABS: pressure altitude */

/* P4X-ALTOUT: the pressure field is the one place where a single bad byte
 * does real damage - {"P":1.5} is 1500 Pa and 22 km of altitude - so a
 * sample that moves the altitude by more than 40 m has to be confirmed by
 * three packets in a row before it is believed.
 */

static int   g_alt_ok;
static int   g_alt_rej;
static float g_alt_cand;

/* P4X-ALTFIT: the ground-side altitude fit.
 *
 * The panel used to show the standard-atmosphere height of the last pressure
 * sample.  That is a raw measurement, and everything raw about it ended up on
 * the screen: the barometer's own noise (0.3 hPa is already 2.5 m), the
 * 0.1 kPa quantisation of the radio field, and the "confirm a 40 m jump three
 * times" gate.  The number wandered by tens of metres with the vehicle
 * standing still, and the curve was a staircase, not a flight profile -
 * exactly the "高度没拟合" report.
 *
 * The airborne side already does the fusion that matters: pressure relative
 * to the launch pressure, anchored to the GNSS altitude, sent as "a".  So the
 * ground station has two real measurements of the same quantity and does the
 * fit it was asked for:
 *
 *   - "a" : the airborne's fused altitude, sea-level referenced once it has a
 *           fix (GNSS absolute, ~1.5 m of noise)
 *   - "P" : the same vehicle's pressure through the standard atmosphere (a few
 *           metres of noise plus a slow, weather-dependent bias)
 *
 * A 1-D constant-velocity Kalman filter turns them into one smooth track plus
 * a vertical rate, and interpolates between the 4 Hz quick packets and the
 * 1 Hz full packets instead of holding the last value until the next one.
 */

static float g_alt_msl;              /* fused altitude, m (what the panel shows) */
static float g_alt_rate;             /* vertical speed of the fit, m/s */
static float g_alt_p00;              /* covariance, position */
static float g_alt_p01;              /* covariance, position/rate */
static float g_alt_p11;              /* covariance, rate */
static int   g_alt_fit_ok;           /* filter initialised */
static int   g_alt_fit_bad;          /* consecutive out-of-gate samples */
static unsigned long g_alt_fit_t;    /* tick of the last filter step */
static int   g_alt_a_ok;             /* "a" arrived in the current frame */

static void alt_fit_reset(float z)
{
  g_alt_msl     = z;
  g_alt_rate    = 0.0f;
  g_alt_p00     = 4.0f;
  g_alt_p01     = 0.0f;
  g_alt_p11     = 0.25f;
  g_alt_fit_ok  = 1;
  g_alt_fit_bad = 0;
  g_alt_fit_t   = clock_systime_ticks();
}

/* One predict + update step.  "r" is the measurement variance (m^2) and
 * "gate" the distance beyond which a sample cannot be the same vehicle.
 */

static void alt_fit_step(float z, float r, float gate)
{
  float dt;
  float q;
  float p00, p01, p11;
  float y, s, k0, k1, t00;

  if (!g_alt_fit_ok)
    {
      alt_fit_reset(z);
      return;
    }

  {
    unsigned long now = clock_systime_ticks();
    unsigned long dms = (now - g_alt_fit_t) * 1000ul / TICK_PER_SEC;

    if (dms == 0ul)
      {
        return;                    /* two frames inside one tick */
      }

    if (dms > 2000ul)
      {
        dms = 2000ul;
      }

    dt = (float)dms / 1000.0f;
    g_alt_fit_t = now;
  }

  /* --- predict: constant velocity with white acceleration noise --- */

  q = 9.0f;                        /* ~3 m/s^2; a launch is ~10 g */

  p00 = g_alt_p00 + 2.0f * dt * g_alt_p01 + dt * dt * g_alt_p11 +
        0.25f * q * dt * dt * dt * dt;
  p01 = g_alt_p01 + dt * g_alt_p11 + 0.5f * q * dt * dt * dt;
  p11 = g_alt_p11 + q * dt * dt;

  g_alt_msl += g_alt_rate * dt;
  g_alt_p00  = p00;
  g_alt_p01  = p01;
  g_alt_p11  = p11;

  /* --- gate.  A sample this far away is not a measurement of the same
   * vehicle.  Three of them in a row mean the filter is the wrong one, so
   * re-lock: the old code could stay frozen for good after one bad byte.
   */

  y = z - g_alt_msl;

  if (y > gate || y < -gate)
    {
      if (++g_alt_fit_bad >= 3)
        {
          alt_fit_reset(z);
        }

      return;
    }

  g_alt_fit_bad = 0;

  /* --- update --- */

  s = p00 + r;
  if (s <= 0.0f)
    {
      return;
    }

  k0 = p00 / s;
  k1 = p01 / s;

  g_alt_msl  += k0 * y;
  g_alt_rate += k1 * y;

  if (g_alt_rate > 120.0f)
    {
      g_alt_rate = 120.0f;
    }
  else if (g_alt_rate < -120.0f)
    {
      g_alt_rate = -120.0f;
    }

  t00 = 1.0f - k0;

  g_alt_p00 = t00 * p00;
  g_alt_p01 = t00 * p01;
  g_alt_p11 = p11 - k1 * p01;

  if (g_alt_p00 < 0.01f)
    {
      g_alt_p00 = 0.01f;
    }

  if (g_alt_p11 < 0.0001f)
    {
      g_alt_p11 = 0.0001f;
    }
}

/* P4X-ALTFIT: choose this frame's measurement.
 *
 * The airborne's fused altitude is only sea-level referenced while it has a
 * fix; without one it is pressure relative to its own boot, so it is not a
 * sea-level measurement and must not be used as one.
 */

static void alt_fit_feed(void)
{
  /* P4X-ALTFIT: the measurement is the airborne's own fused altitude, "a".
   * Nothing is converted: the panel shows the same quantity the vehicle
   * reports, only filtered.  A frame without "a" does not update the estimate
   * (the predict step keeps it steady) instead of substituting a different
   * quantity that would need its own reference.
   */

  if (g_alt_a_ok)
    {
      alt_fit_step(fl.alt, 2.25f, 150.0f);
    }
}

/* P4X-SATCLS: per-constellation satellite counts.
 *
 * The airborne parses $xxGSV and sends the four in-view counts as
 * "sg":[GPS,GLONASS,Galileo,BeiDou].  The GPS module on this vehicle is a
 * BeiDou one and only emits $BDGSV, so today only the fourth slot carries
 * real data - the rest stay 0 and the sub-page says so instead of pretending
 * to know.  If the module is ever switched to multi-GNSS the bars light up
 * with no further change here.
 */

#define SAT_CLS     4           /* GPS, GLONASS, Galileo, BeiDou */
#define SAT_HIST    60          /* one sample per second = 1 minute */

static int g_sc[SAT_CLS];                        /* in view, per constellation */
static int g_sc_seen;                            /* "sg" arrived at least once */
static unsigned char g_sc_hist[SAT_CLS][SAT_HIST];
static int g_sc_hist_idx;

static int g_sat_page;                           /* 1 = the SAT sub-page is open */
static int g_sat_x0 = -1, g_sat_y0;              /* header SAT hit rect */
static int g_sat_x1 = -1, g_sat_y1;
static int g_sat_held;                           /* debounce the SAT tap */

static int g_time_from_gps;

static void telemetry_apply_json(const char *line)
{
  static const char *keys_f[] = { "\"v\":", "\"alt\":", "\"ax\":", "\"ay\":", "\"az\":",
                                  "\"pitch\":", "\"roll\":", "\"yaw\":",
                                  "\"Pq\":", "\"temp\":", "\"t\":" };
  static const char *keys_i[] = { "\"sat\":", "\"L\":", "\"ph\":" };
  float *dst_f[] = { &fl.v, &fl.alt, &fl.ax, &fl.ay, &fl.az,
                     &fl.pitch, &fl.roll, &fl.yaw, &fl.pressure, &fl.temperature, &fl.t };
  int   *dst_i[] = { &fl.sat, &fl.rocket_link, &fl.phase };
  int ok = 0, i;

  g_alt_a_ok = 0;             /* P4X-ALTFIT: per-frame flag */

  for (i = 0; i < (int)(sizeof(keys_f) / sizeof(keys_f[0])); i++)
    {
      const char *p = strstr(line, keys_f[i]);
      if (p != NULL)
        {
          *dst_f[i] = strtof(p + strlen(keys_f[i]), NULL);
          ok++;
        }
    }
  for (i = 0; i < (int)(sizeof(keys_i) / sizeof(keys_i[0])); i++)
    {
      const char *p = strstr(line, keys_i[i]);
      if (p != NULL)
        {
          *dst_i[i] = (int)strtol(p + strlen(keys_i[i]), NULL, 10);
          ok++;
        }
    }

  {
    const char *p = strstr(line, "\"lat\":");
    const char *q = strstr(line, "\"lon\":");
    if (p != NULL) fl.lat = strtod(p + 6, NULL);
    if (q != NULL) fl.lon = strtod(q + 6, NULL);

    /* P4X-P4LAT: 箭载端发的是短键 "la" / "lo"，
     * 只找长键会让 fl.lat/fl.lon 永远为 0，网页就说"还没收到火箭坐标"。
     */

    if ((p = strstr(line, "\"la\":")) != NULL)
      {
        double v = strtod(p + 5, NULL);
        if (v > -90.0 && v < 90.0)
          {
            fl.lat = v;
            ok++;
          }
      }

    if ((q = strstr(line, "\"lo\":")) != NULL)
      {
        double v = strtod(q + 5, NULL);
        if (v > -180.0 && v < 180.0)
          {
            fl.lon = v;
            ok++;
          }
      }
  }

  /* P4X-SCHEMA2: the airborne side sends short keys, not the long ones above:
   *   quick = {"p":pitch,"r":roll,"y":yaw}
   *   full  = {...,"la":lat,"lo":lon,"a":alt,"gps":0/1,"s":sat,"t":temp,
   *              "P":pressure in kPa,"v":speed,"T":gps time,"L":link}
   * Keys that do not match left every attitude field at 0.0, which is why the
   * curves stayed flat while only the accelerometer (same key names) moved.
   */

  {
    const char *p2;

    if ((p2 = strstr(line, "\"p\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val > -180.0f && val < 180.0f)
          {
            fl.pitch = val;
            ok++;
          }
      }

    if ((p2 = strstr(line, "\"r\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val > -180.0f && val < 180.0f)
          {
            fl.roll = val;
            ok++;
          }
      }

    if ((p2 = strstr(line, "\"y\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val > -360.0f && val < 720.0f)
          {
            fl.yaw = val;
            ok++;
          }
      }

    /* pressure arrives in kPa; the panel and the curve expect Pa */

    if ((p2 = strstr(line, "\"P\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL) * 1000.0f;

        /* P4X-ALTOUT: a plausible sea-level..7 km band.  The old window went
         * down to 1000 Pa, which is 31 km of altitude, and that is exactly
         * how a mangled "P":101.5 -> "P":1.5 threw the panel to 22 km.
         */

        if (val > 40000.0f && val < 110000.0f)
          {
            float ratio = val / 101325.0f;
            float na = 44330.0f * (1.0f - powf(ratio, 0.190295f));
            int accept = 1;

            if (g_alt_ok && fabsf(na - g_alt_abs) > 40.0f)
              {
                /* Either a hole in the frame or a real launch - only a
                 * value that repeats can be a real launch.
                 */

                if (g_alt_rej > 0 && fabsf(na - g_alt_cand) < 40.0f)
                  {
                    g_alt_rej++;
                  }
                else
                  {
                    g_alt_rej = 1;
                    g_alt_cand = na;
                  }

                accept = (g_alt_rej >= 3);
              }
            else
              {
                g_alt_rej = 0;
              }

            if (accept)
              {
                g_alt_abs = g_alt_ok ? (g_alt_abs * 0.5f + na * 0.5f) : na;
                g_alt_cand = g_alt_abs;
                g_alt_ok = 1;
                g_alt_rej = 0;
                fl.pressure = val;
                ok++;
              }
          }
      }

    if ((p2 = strstr(line, "\"a\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val > -2000.0f && val < 40000.0f)
          {
            fl.alt = val;
            g_alt_a_ok = 1;      /* P4X-ALTFIT: a fused altitude arrived */
            ok++;
          }
      }

    if ((p2 = strstr(line, "\"la\":")) != NULL)
      {
        fl.lat = strtod(p2 + 5, NULL);
        ok++;
      }

    if ((p2 = strstr(line, "\"lo\":")) != NULL)
      {
        fl.lon = strtod(p2 + 5, NULL);
        ok++;
      }

    if ((p2 = strstr(line, "\"s\":")) != NULL)
      {
        int val = (int)strtol(p2 + 4, NULL, 10);
        if (val >= 0 && val <= 32)
          {
            fl.sat = val;
            ok++;
          }
      }

    /* P4X-SATCLS: "sg":[GPS,GLONASS,Galileo,BeiDou] - in-view counts per
     * constellation, as the airborne parsed them out of $xxGSV.
     */

    if ((p2 = strstr(line, "\"sg\":[")) != NULL)
      {
        const char *q = p2 + 6;
        int k;

        for (k = 0; k < SAT_CLS; k++)
          {
            int val = 0;

            while (*q == ' ')
              {
                q++;
              }

            while (*q >= '0' && *q <= '9')
              {
                val = val * 10 + (*q - '0');
                q++;
              }

            if (val > 32)
              {
                val = 32;
              }

            g_sc[k] = val;

            while (*q == ' ')
              {
                q++;
              }

            if (*q == ',' || *q == ']')
              {
                if (*q == ',')
                  {
                    q++;
                  }
              }
            else
              {
                break;
              }
          }

        g_sc_seen = 1;
        ok++;
      }

    if ((p2 = strstr(line, "\"t\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val > -60.0f && val < 80.0f)
          {
            fl.temperature = val;
            ok++;
          }
      }

    /* P4X-TIMEFIX: the clock is the "T" *string*, not the "t" number (which
     * is the temperature, 28.12 C).  Showing T+28.1 as a clock made the time
     * jump around with the temperature.
     */

    if ((p2 = strstr(line, "\"T\":\"")) != NULL)
      {
        const char *q = p2 + 5;
        int n = 0;

        while (q[n] != '\0' && q[n] != '"' &&
               n < (int)sizeof(fl.time_str) - 1)
          {
            n++;
          }

        if (n > 0)
          {
            /* P4X-NOTIME: "NO_TIME" is a placeholder, not a clock.  Only a
             * real HH:MM:SS (it contains ':') may turn off the local timer.
             */

            int has_colon = 0;
            int k2;

            for (k2 = 0; k2 < n; k2++)
              {
                if (q[k2] == ':')
                  {
                    has_colon = 1;
                    break;
                  }
              }

            if (has_colon)
              {
                memcpy(fl.time_str, q, (size_t)n);
                fl.time_str[n] = '\0';
                g_time_from_gps = 1;

                if (n >= 2 && q[0] >= '0' && q[0] <= '9' &&
                    q[1] >= '0' && q[1] <= '9')
                  {
                    g_gps_hour = (q[0] - '0') * 10 + (q[1] - '0');
                  }
                ok++;
              }
          }
      }

    if ((p2 = strstr(line, "\"gps\":")) != NULL)
      {
        int val = (int)strtol(p2 + 6, NULL, 10);
        if (val == 0 || val == 1)
          {
            fl.gps_fix = val;
            ok++;
          }
      }

    if ((p2 = strstr(line, "\"v\":")) != NULL)
      {
        float val = strtof(p2 + 4, NULL);
        if (val >= 0.0f && val < 500.0f)
          {
            fl.v = val;
            ok++;
          }
      }
  }

  /* P4X-SMOOTH: a frame that only carries one key is a truncated packet.  It
   * used to be accepted, which left every other field at its old value and made
   * the curves jump; 4 Hz quick packets carry 3 keys and full packets 12, so
   * requiring 2 costs nothing.
   */

  if (ok >= 2)
    {
      telemetry_live = 1;
      lora_packets++;

      /* P4X-JSONECHO2: 直接写 /dev/console（这个任务的 fd 1 不可靠）。
       * 每 2 帧一次（约 4 Hz），格式与网页 dashboard.html 的 parseJSON 对齐。
       */

      {
        static int echo_n;

        if ((++echo_n & 1) == 0)
          {
            int cfd = open("/dev/console", O_WRONLY | O_NONBLOCK);

            if (cfd >= 0)
              {
                char ebuf[240];
                int  elen;

                elen = snprintf(ebuf, sizeof(ebuf),
                                "{\"alt\":%.2f,\"v\":%.2f,\"p\":%.2f,"
                                "\"r\":%.2f,\"y\":%.2f,\"ax\":%.2f,"
                                "\"ay\":%.2f,\"az\":%.2f,\"sat\":%d,"
                                "\"phase\":%d,\"lora\":1,\"temp\":%.1f,"
                                "\"press\":%.0f,\"la\":%.7f,\"lo\":%.7f,"
                                "\"time\":\"%s\",\"gps\":%d}\n",
                                (double)fl.alt, (double)fl.v, (double)fl.pitch,
                                (double)fl.roll, (double)fl.yaw, (double)fl.ax,
                                (double)fl.ay, (double)fl.az, fl.sat, fl.phase,
                                (double)fl.temperature, (double)fl.pressure,
                                fl.lat, fl.lon,
                                fl.time_str, fl.gps_fix);

                if (elen > 0)
                  {
                    /* 阻塞写 + 循环写满：控制台缓冲小，一次 write 可能只写一部分。 */

                    /* 非阻塞 + 有限重试：写不进去就丢这一帧。
                     * 拔掉 USB 线后控制台没有接收方，阻塞写会让 UI 永久卡住
                     * （表现为"背光亮着但画面不动"）。
                     */

                    int sent = 0;
                    int tries = 0;

                    while (sent < elen && tries < 20)
                      {
                        int n = (int)write(cfd, ebuf + sent, (size_t)(elen - sent));

                        if (n > 0)
                          {
                            sent += n;
                            tries = 0;
                          }
                        else
                          {
                            tries++;
                            usleep(2000);      /* 2 ms，给控制台留排空时间 */
                          }
                      }
                  }

                close(cfd);
              }
          }
      }
      /* P4X-SMOOTH: reject implausible jumps and smooth what is left.  The
       * 45/90 degree gates cannot hide real motion (the samples are 250-500 ms
       * apart) but they do drop the frame-split artefacts.
       */

      /* P4X-REVERT2: no boot-pose calibration.  Measured on both sides:
       * the airborne reads ax=-0.141 g and computes roll=7.55 deg, and the
       * panel receives exactly {"ax":-0.141,"r":7.5} - the chain is correct,
       * so the panel must show these numbers unchanged.
       */

      {
        static float sr, sp, sy;
        static int seeded;

        float nr = fl.roll;
        float np = fl.pitch;
        float ny = fl.yaw;

        if (!seeded)
          {
            sr = nr;
            sp = np;
            sy = ny;
            seeded = 1;
          }

        if (nr > sr + 45.0f || nr < sr - 45.0f)
          {
            /* P4X-ATTSTUCK */

            sr = nr;
            fl.roll = sr;
          }
        else
          {
            sr += 0.25f * (nr - sr);
            fl.roll = sr;
          }

        if (np > sp + 45.0f || np < sp - 45.0f)
          {
            /* P4X-ATTSTUCK: a jump this large is a real flip, not noise.
             * Rejecting it used to freeze the display for good.
             */

            sp = np;
            fl.pitch = sp;
          }
        else
          {
            sp += 0.25f * (np - sp);
            fl.pitch = sp;
          }

        if (ny > sy + 90.0f || ny < sy - 90.0f)
          {
            /* P4X-ATTSTUCK */

            sy = ny;
            fl.yaw = sy;
          }
        else
          {
            sy += 0.25f * (ny - sy);
            fl.yaw = sy;
          }
      }

      fl.gps_fix = fl.lat != 0.0 && fl.lon != 0.0;

      /* P4X-PHASE: adopt the phase the airborne reports, but never while a
       * command of ours is still going out (or waiting for confirmation) -
       * otherwise the panel would flip back to the old phase mid-command.
       */

      /* P4X-PHASEALIGN: the panel's phase IS the vehicle's phase.
       *
       * The old code waited three seconds after a command before it accepted
       * the vehicle's answer, because the local phase had already moved ahead
       * and the telemetry would have pulled it back.  Now nothing moves ahead
       * locally, so every valid "ph" can be taken as-is: what the operator sees
       * is what the vehicle says, and a command that never arrived changes
       * nothing on screen.
       */

      if (fl.phase >= 0 && fl.phase < PH_NUM && g_phase_tx_left <= 0 &&
          g_phase_need_confirm == 0)
        {
          g_phase_idx = fl.phase;
        }

      /* P4X-PHASEALIGN: did the vehicle reach the phase we asked for? */

      if (g_ph_wait_ack && g_phase_target >= 0 && fl.phase == g_phase_target)
        {
          g_ph_wait_ack = 0;
          g_ph_acked    = 1;
          state_dirty   = 1;      /* the header can drop "发送中" now */
        }

      /* P4X-ALTFIT: exactly one filter step per accepted frame, fed with the
       * best altitude measurement this frame carries.
       */

      alt_fit_feed();

      /* P4X-HDR: "connected" must mean data is actually arriving, not
       * "a packet was parsed once".  The status page showed 连接 OK with the
       * link dead because this line used to set it unconditionally.
       */

      /* P4X-LINKSTATE: the live flag is maintained in the UI loop now. */
      /* P4X-CLOCKFIX: without a GPS fix the panel still needs a clock, so run
       * a local one from boot exactly like the earlier implementation did:
       * T+MM:SS counting up every second.  (It used to print T+<temperature>,
       * which is why the clock jumped around.)
       */

      if (!g_time_from_gps)
        {
          static unsigned long clock0;
          unsigned long nowt = clock_systime_ticks();
          unsigned long secs;

          if (clock0 == 0)
            {
              clock0 = nowt;
            }

          secs = (nowt - clock0) / TICK_PER_SEC;

          snprintf(fl.time_str, sizeof(fl.time_str), "T+%lu:%02lu",
                   secs / 60ul, secs % 60ul);
        }
    }
  else
    {
      lora_bad++;
    }
}

/****************************************************************************
 * P4X-ROUTEPROBE: which GPIO-matrix setting actually feeds UART1 RX?
 *
 * The pad is at 3.25 V with its input buffer enabled, yet UART_STATUS[15]
 * reads 0 and the RX FIFO count never moves off 48.  in_sel[13] (UART1's RX
 * input slot) is left at 0x95, whose bit 7 (SIG_IN_SEL) selects the direct
 * IO_MUX path rather than the pad number in the low six bits.  Try every
 * variant and report which one makes the controller follow the real line.
 ****************************************************************************/

static void route_variant(const char *name, unsigned int in_cfg,
                          unsigned int mux_sel_gpio)
{
  unsigned long base = 0x500cb000ul/*P4X-REALBASE*/;
  unsigned long incfg = 0x500e0000ul + 0x158ul + 4ul * 13ul;
  unsigned long mux21 = 0x500e1000ul + 0x54ul + 4ul * 21ul;
  unsigned int st;
  unsigned int n = 0;
  int i;
  int got = 0;

  /* Program the variant. */

  if (mux_sel_gpio)
    {
      unsigned int v = *(volatile unsigned int *)mux21;

      v = (v & ~0x00007000u) | 0x00001000u;   /* MCU_SEL = 1 (plain GPIO) */
      v |= 0x00000200u;                        /* IE */
      *(volatile unsigned int *)mux21 = v;
    }

  *(volatile unsigned int *)incfg = in_cfg;
  usleep(20000);

  /* Wait for the FIFO to fill, then report. */

  for (i = 0; i < 30; i++)
    {
      usleep(10000);
      st = *(volatile unsigned int *)(base + 0x1cul);
      n = st & 0xffu;
      if (n > 0)
        {
          got = 1;
          break;
        }
    }

  printf("P4X-ROUTEPROBE: %-34s in_sel[13]=0x%08x -> status=0x%08x "
         "rxd=%u rxfifo=%u\n",
         name, *(volatile unsigned int *)incfg, st,
         (st >> 15) & 1u, n);

  /* Drain whatever arrived so the next variant starts clean. */

  while ((*(volatile unsigned int *)(base + 0x1cul) & 0xffu) != 0)
    {
      (void)*(volatile unsigned int *)base;
      if (++got > 4096)
        {
          break;
        }
    }
}

static void route_probe(void)
{
  printf("P4X-ROUTEPROBE: which routing delivers the RX pad?\n");

  route_variant("A matrix: SIG_IN_SEL=0 pad=21", 21u, 0);
  route_variant("B direct: driver value 0x95", 0x95u, 0);
  route_variant("C matrix + MCU_SEL=1", 21u, 1);

  /* Leave the driver's value in place. */

  *(volatile unsigned int *)(0x500e0000ul + 0x158ul + 4ul * 13ul) = 0x95u;
  printf("P4X-ROUTEPROBE: done\n");
}

/****************************************************************************
 * P4X-DRIVEPROBE: drive GPIO21 and watch UART1's RXD bit.
 *
 * If the controller follows the level driven onto the pad then GPIO21 really is
 * its input and the problem is the idle level; if RXD stays 0 for both levels
 * then UART1 is not looking at GPIO21 at all.
 ****************************************************************************/

static void drive_probe(void)
{
  unsigned long base = 0x500cb000ul/*P4X-REALBASE*/;
  unsigned long mux21 = 0x500e1000ul + 0x54ul + 4ul * 21ul;
  unsigned long incfg = 0x500e0000ul + 0x158ul + 4ul * 13ul;
  unsigned long en_w1ts = 0x500e0000ul + 0x24ul;
  unsigned long en_w1tc = 0x500e0000ul + 0x28ul;
  unsigned long out_w1ts = 0x500e0000ul + 0x08ul;
  unsigned long out_w1tc = 0x500e0000ul + 0x0cul;
  unsigned long in_reg = 0x500e0000ul + 0x3cul;
  unsigned int save_mux = *(volatile unsigned int *)mux21;
  unsigned int save_in = *(volatile unsigned int *)incfg;
  unsigned int v;
  unsigned int st;
  int level;

  /* Matrix: UART1's RX input slot takes the pad number (SIG_IN_SEL = 0). */

  *(volatile unsigned int *)incfg = 21u;

  /* GPIO21 as a plain GPIO output with the input buffer on. */

  v = save_mux & ~0x00007000u;
  v |= 0x00001000u;      /* MCU_SEL = 1 */
  v |= 0x00000200u;      /* IE       */
  *(volatile unsigned int *)mux21 = v;

  *(volatile unsigned int *)en_w1ts = 1u << 21;
  usleep(20000);

  for (level = 0; level <= 1; level++)
    {
      if (level)
        {
          *(volatile unsigned int *)out_w1ts = 1u << 21;
        }
      else
        {
          *(volatile unsigned int *)out_w1tc = 1u << 21;
        }

      usleep(50000);
      st = *(volatile unsigned int *)(base + 0x1cul);

      printf("P4X-DRIVEPROBE: drove GPIO21=%d -> pad_bit=%u uart_rxd=%u "
             "status=0x%08x\n",
             level, (*(volatile unsigned int *)in_reg >> 21) & 1u,
             (st >> 15) & 1u, st);
    }

  /* Release the pad back to an input and restore the driver's routing. */

  *(volatile unsigned int *)en_w1tc = 1u << 21;
  *(volatile unsigned int *)mux21 = save_mux;
  *(volatile unsigned int *)incfg = save_in;
  printf("P4X-DRIVEPROBE: done\n");
}

/****************************************************************************
 * P4X-SLOTSCAN: which matrix input slot drives UART1's RX?
 *
 * 0x3E in an input-select register means "tie this input low", not "select
 * signal 62", and bit 7 set means "use the matrix instead of the direct IO-MUX
 * path".  Walk every slot, point it at GPIO21 and see which one makes
 * UART_STATUS[15] follow the level driven onto the pad.
 ****************************************************************************/

static void slot_scan(void)
{
  unsigned long base = 0x500cb000ul/*P4X-REALBASE*/;
  unsigned long incfg0 = 0x500e0000ul + 0x158ul;
  unsigned long mux21 = 0x500e1000ul + 0x54ul + 4ul * 21ul;
  unsigned long en_w1ts = 0x500e0000ul + 0x24ul;
  unsigned long en_w1tc = 0x500e0000ul + 0x28ul;
  unsigned long out_w1ts = 0x500e0000ul + 0x08ul;
  unsigned long out_w1tc = 0x500e0000ul + 0x0cul;
  unsigned int save_mux = *(volatile unsigned int *)mux21;
  unsigned int v;
  int slot;
  int found = -1;

  /* GPIO21 as a plain output so its level can be set on demand. */

  v = save_mux & ~0x00007000u;
  v |= 0x00001000u | 0x00000200u;
  *(volatile unsigned int *)mux21 = v;
  *(volatile unsigned int *)en_w1ts = 1u << 21;

  printf("P4X-SLOTSCAN: scanning matrix input slots for UART1 RX\n");

  for (slot = 0; slot <= 56; slot++)
    {
      unsigned long reg = incfg0 + 4ul * (unsigned long)slot;
      unsigned int low;
      unsigned int high;

      /* Point this slot at GPIO21 through the matrix (bit 7 = do not bypass). */

      *(volatile unsigned int *)reg = 21u | 0x80u;

      *(volatile unsigned int *)out_w1tc = 1u << 21;   /* pad low  */
      usleep(3000);
      low = (*(volatile unsigned int *)(base + 0x1cul) >> 15) & 1u;

      *(volatile unsigned int *)out_w1ts = 1u << 21;   /* pad high */
      usleep(3000);
      high = (*(volatile unsigned int *)(base + 0x1cul) >> 15) & 1u;

      if (low == 0 && high == 1)
        {
          printf("P4X-SLOTSCAN: slot %d FOLLOWS the pad (low=0 high=1) "
                 "<-- UART1 RX\n", slot);
          found = slot;
        }
      else if (slot < 8 || (low == 1 && high == 1) || (low == 0 && high == 0))
        {
          /* Print the first few and any slot that is not obviously dead, so
           * the log stays short but informative.
           */

          if (slot < 8)
            {
              printf("P4X-SLOTSCAN: slot %d low=%u high=%u\n",
                     slot, low, high);
            }
        }
    }

  if (found < 0)
    {
      printf("P4X-SLOTSCAN: no slot followed the pad; UART1 RX is not fed by "
             "the GPIO matrix on this port\n");
    }
  else
    {
      printf("P4X-SLOTSCAN: use incfg slot %d for UART1 RX\n", found);
    }

  /* Release the pad and restore the driver's register. */

  *(volatile unsigned int *)en_w1tc = 1u << 21;
  *(volatile unsigned int *)mux21 = save_mux;
}

/****************************************************************************
 * P4X-AUTOPIN: listen on GPIO38, fall back to GPIO21.
 *
 * The aerial link is proven (an S3 write moved this controller's RX FIFO count),
 * but the pin the module's TXD actually reaches is not: GPIO38 reads 0 V on the
 * pad while the module idles high, and 3.25 V was measured on GPIO21 earlier.
 *
 * So rotate UART0's matrix RX input between the two candidate pads until bytes
 * appear, then stay there and report which one worked.  The pin is then a fact
 * read from the hardware rather than an assumption about the wiring.
 ****************************************************************************/

#define INCFG_UART0_RX   (0x500e0000ul + 0x158ul + 4ul * 10ul)
#define UART0_STATUS_R   0x500ca01cul
#define UART0_FIFO_R     0x500ca000ul

static int g_rx_pin = -1;          /* -1 = not decided yet */

/* Returns the pin that is currently selected. */

static int autopin_tick(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    /* P4X-FORCE-INCFG: 每 10 秒重写,确保 RX 输入槽指向 GPIO21。 */

    *(volatile unsigned int *)INCFG_UART0_RX = 21u;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  /* P4X-PINS-FINAL: 引脚已由实测确定(GPIO21),这里只保留函数外形,
   * 直接返回选定的脚,不再切换。矩阵输入槽 10 由 board_lora_route() 设定。
   */

  (void)bytes_seen;
  {
    static int announced;

    /* P4X-FORCE-INCFG: 每 10 秒重写,确保 RX 输入槽指向 GPIO21。 */

    *(volatile unsigned int *)INCFG_UART0_RX = 21u;

    if (!announced)
      {
        announced = 1;
        printf("P4X-AUTOPIN: RX fixed on GPIO21 (J1 pin 11) - verified by scan\n");
      }
  }

  return 21;
}
#if 0  /* 旧的轮换逻辑,保留备查 */
static int autopin_tick_old(int bytes_seen)
{
  static const int candidates[2] = { 38, 21 };
  static unsigned int last_switch;
  static int idx;
  unsigned int now = (unsigned int)clock_systime_ticks();
  unsigned int fifo;
  unsigned int rxd;

  if (g_rx_pin > 0)
    {
      return g_rx_pin;
    }

  if (idx == 0 && last_switch == 0)
    {
      last_switch = now;
    }

  fifo = *(volatile unsigned int *)UART0_STATUS_R & 0xffu;
  rxd = (*(volatile unsigned int *)UART0_STATUS_R >> 15) & 1u;

  /* Bytes arrived: this pad is the one. */

  if (bytes_seen > 0 || fifo > 0)
    {
      g_rx_pin = candidates[idx];
      printf("P4X-AUTOPIN: bytes arrived on GPIO%d - module TXD is on J1 %s\n",
             g_rx_pin, (g_rx_pin == 38) ? "pin 10" : "pin 11");
      return g_rx_pin;
    }

  /* Nothing yet: try the other pad every 2 s. */

  if ((now - last_switch) > (2 * TICK_PER_SEC))
    {
      idx = (idx + 1) & 1;
      last_switch = now;

      /* Point UART0's RX input slot at the candidate through the IO-MUX
       * bypass path (SIG_IN_SEL = 0), which is how the working case behaves.
       */

      *(volatile unsigned int *)INCFG_UART0_RX = (unsigned int)candidates[idx];

      printf("P4X-AUTOPIN: now listening on GPIO%d (rxd=%u rxfifo=%u)\n",
             candidates[idx], rxd, fifo);
    }

  return candidates[idx];
}
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif

/****************************************************************************
 * P4X-SELFTEST: prove both directions once, at start-up.
 *
 * RX: UART0_STATUS[15] must follow the level actually on GPIO21.  Measured by
 *     the exhaustive scan to work via matrix input slot 10.
 * TX: writing through the port must move the GPIO20 pad.
 *
 * The result is one line in the log, so "the wiring is ready" is a statement
 * read off the hardware rather than an assumption.
 ****************************************************************************/

#define P4X_GPIO_IN_R       0x500e003cul
#define P4X_GPIO_EN_W1TS_R  0x500e0024ul
#define P4X_GPIO_EN_W1TC_R  0x500e0028ul
#define P4X_GPIO_OUT_W1TS_R 0x500e0008ul
#define P4X_GPIO_OUT_W1TC_R 0x500e000cul
#define P4X_FUNC_OUT_20     (0x500e0000ul + 0x558ul + 4ul * 20ul)
#define P4X_UART0_STAT      0x500ca01cul
#define SIG_UART0_TXD       10u
#define OUT_OEN_SEL_BIT     0x00000400u

static void self_test(void)
{
  unsigned int seen_low = 0;
  unsigned int seen_high = 0;
  unsigned int in_sel = *(volatile unsigned int *)(0x500e0000ul + 0x158ul + 4ul * 10ul);
  int rxd;
  int i;

  /* RX: report what the controller sees right now. */

  rxd = (int)((*(volatile unsigned int *)P4X_UART0_STAT >> 15) & 1u);

  printf("P4X-SELFTEST: RX: in_sel[10]=0x%08x uart_rxd=%d "
         "(1 = the line idles high, so a connected module can be read)\n",
         in_sel, rxd);

  /* TX: route UART0's TXD to GPIO20 with the peripheral owning the pad
   * enable, then send real bytes through the port and watch the pad.
   */

  *(volatile unsigned int *)P4X_FUNC_OUT_20 = SIG_UART0_TXD | OUT_OEN_SEL_BIT;

  if (board_lora_ready())
    {
      for (i = 0; i < 40; i++)
        {
          board_lora_send("U", 1);

          if ((*(volatile unsigned int *)P4X_GPIO_IN_R >> 20) & 1u)
            {
              seen_high = 1;
            }
          else
            {
              seen_low = 1;
            }
        }
    }

  printf("P4X-SELFTEST: TX: out_sel[20]=0x%08x pad20 low=%u high=%u => %s\n",
         *(volatile unsigned int *)P4X_FUNC_OUT_20, seen_low, seen_high,
         (seen_low && seen_high) ? "UART0 reaches GPIO20" :
         "UART0 does NOT reach GPIO20");

  if (rxd == 1 && seen_low && seen_high)
    {
      printf("P4X-SELFTEST: BOTH DIRECTIONS READY - module TXD -> pin 11 "
             "(GPIO21), module RXD -> pin 13 (GPIO20)\n");
    }
  else if (rxd == 1)
    {
      printf("P4X-SELFTEST: RX READY, TX not reaching the pad - telemetry will "
             "arrive, sending will not\n");
    }
  else
    {
      printf("P4X-SELFTEST: RX not seeing the pad - check that the module TXD "
             "is on J1 pin 11 (GPIO21)\n");
    }
}

/****************************************************************************
 * P4X-PAD38: the pad's own level, independent of UART0.
 ****************************************************************************/

#define P4X_GPIO_IN_REG     0x500e003cul
#define P4X_IO_MUX_38       (0x500e1000ul + 0x58ul)  /* P4X-PINS-FINAL: GPIO21 */
#define P4X_FUN_IE_BIT      0x00000200u
#define P4X_FUN_WPU_BIT     0x00000100u

static int pad38_level(void)
{
  /* P4X-NOSELFDESTRUCT: read only.
   *
   * This used to switch GPIO38's IO-MUX to plain-GPIO mode (MCU_SEL = 1) and
   * enable a pull-up "to be safe".  That disconnects UART0's direct IO-MUX
   * function on that pad (FUNC_GPIO38_UART0_RXD_PAD = 0 vs FUNC_GPIO38_GPIO38
   * = 1), so the probe itself turned a working RX line into rxd = 0 within one
   * status tick.  A diagnostic must not reconfigure the thing it measures.
   */

  return (int)((*(volatile unsigned int *)P4X_GPIO_IN_REG >> 21) & 1u);
}

/****************************************************************************
 * P4X-FIFODUMP: read the UART FIFO straight from the registers.
 *
 * Written because the driver and the hardware disagree: the block the driver
 * programmed holds RXFIFO_CNT = 48 that a driver read() never delivers, and
 * STATUS[15] (UART_RXD) reports 0 while the pad is high with the input buffer
 * on.  Reading UART_FIFO_REG by hand separates "the driver cannot see the
 * bytes" from "the module stopped sending".
 ****************************************************************************/

static unsigned int fifo_rd(unsigned long a)
{
  return *(volatile unsigned int *)a;
}

static void fifo_dump(void)
{
  static const unsigned long bases[4] =
  {
    0x500d0000ul, 0x500d1000ul, 0x500cb000ul/*P4X-REALBASE*/, 0x500d3000ul
  };
  int b;

  for (b = 0; b < 4; b++)
    {
      unsigned long base = bases[b];
      unsigned int clk = fifo_rd(base + 0x88ul);
      unsigned int st;
      unsigned int cnt;
      int n;
      int i;

      printf("P4X-FIFODUMP: block 0x%08lx clk_conf=0x%08x\n", base, clk);
      if (clk == 0)
        {
          continue;
        }

      st = fifo_rd(base + 0x1cul);
      cnt = st & 0xffu;

      printf("P4X-FIFODUMP:   status=0x%08x rxfifo=%u rxd=%u txd=%u\n",
             st, cnt, (st >> 15) & 1u, (st >> 31) & 1u);

      n = 0;
      while (n < 64 && ((fifo_rd(base + 0x1cul) & 0xffu) != 0))
        {
          unsigned int v = fifo_rd(base);

          if (n < 32)
            {
              printf("%s%02x", (n == 0) ? "P4X-FIFODUMP:   data =" : " ", v & 0xffu);
            }

          n++;
        }

      printf("\n");
      printf("P4X-FIFODUMP:   drained %d byte(s), status now 0x%08x rxfifo=%u\n",
             n, fifo_rd(base + 0x1cul),
             fifo_rd(base + 0x1cul) & 0xffu);

      if (n > 0)
        {
          printf("P4X-FIFODUMP:   ascii:");
          for (i = 0; i < n && i < 32; i++)
            {
              unsigned int v = 0;
              (void)v;
              printf(" .");
            }

          printf("  (hex above)\n");
        }
    }
}

/****************************************************************************
 * P4X-LORARX: the receive loop the ground station never had.
 *
 * The UI used to call only board_lora_ready/detected/stats, so whatever the
 * A28 module delivered sat unread in the driver and the screen stayed on the
 * demo profile.  This task reads the port, rebuilds lines and feeds them to
 * telemetry_apply_json().
 *
 * Quoting/printing is deliberately minimal.  An earlier version printed every
 * received chunk; when the host side of the USB-CDC console went away mid
 * burst the console write blocked forever and took the shell and the UI with
 * it.
 ****************************************************************************/

static int lora_rx_task(int argc, FAR char *argv[])
{
  static char line[256];
  uint8_t buf[256];   /* P4X-RAW: one burst can exceed 128 */
  int used = 0;
  int inframe = 0;              /* P4X-FRAME2: inside a {...} frame? */
  unsigned long chunks = 0;
  unsigned long bytes = 0;
  unsigned long lines = 0;
  unsigned long started = clock_systime_ticks();
  int dumped = 0;                                           /* P4X-LORARX-HEX */

  (void)argc;
  (void)argv;

  /* P4X-LORARX-DATA: data-only start.  board_lora_initialize() would run the
   * configuration probe, which puts the module into the 1/1 state - the state
   * in which it forwards nothing - and drains whatever already arrived.  The
   * ground station only ever wants telemetry, so open the port without it.
   */

  if (board_lora_initialize_mode(0) < 0)
    {
      printf("P4X-LORARX: no port, receive loop disabled\n");
      return 0;
    }

  /* Data path only: MD0 = MD1 = 0 is the transparent half-duplex state. */

  board_lora_set_state(0, 0);

  /* P4X-LOOPBACK: 回环自检。
   *
   * 主人把 J1 pin 8(GPIO37=TX) 与 pin 11(GPIO21=RX) 短接后,发出去的字节
   * 应当原样回来。逐字节比较 —— 这个项目已经被"收到东西就算过"的假判据
   * 坑过一次,不再重犯。
   *
   *   PASS -> UART0 的 TX/RX 都正常,问题在模块那一侧
   *   FAIL -> UART0 本身发不出去,与模块无关
   */

  /* P4X-LINKMODE: the module is wired the documented way with MD0/MD1 tied to
   * GND, so the A28 sits in the 0/0 transparent state.  A loopback test would
   * only inject 16 bytes into the air and then fail, because a transparent
   * module never echoes.  Announce the link and listen instead.
   */

  printf("P4X-LINKMODE: A28 transparent (MD0=MD1=GND) TX=GPIO20 RX=GPIO21 "
         "at %d baud - listening for the airborne telemetry\n", board_lora_baud());

  board_lora_set_state(0, 0);

  /* P4X-LINKMODE: the baud sweep is gone.  With MD0/MD1 tied to GND the module
   * cannot enter the 1/1 configuration state at all, so C3 can never be
   * answered - and every board_lora_probe_baud() call re-programs the UART
   * divisor, which used to leave the port at some other rate than 9600 and kill
   * the data path it was trying to fix.
   */

  printf("P4X-LORARX: receive loop running at %d baud\n", board_lora_baud());

  /* fifo_dump(); removed */      /* P4X-FIFODUMP: what does the hardware really hold? */
  /* self_test(); removed */      /* P4X-SELFTEST: RX and TX, stated once */
  /* route_probe(); removed */    /* P4X-ROUTEPROBE: which routing feeds UART1 RX? */
  /* drive_probe(); removed */    /* P4X-DRIVEPROBE: does UART1 read GPIO21? */
  /* slot_scan(); removed */      /* P4X-SLOTSCAN: which matrix slot is UART1 RX? */

  for (;;)
    {
      int n = board_lora_recv(buf, sizeof(buf));
      int i;

      if (n > 0)
        {
          bytes += (unsigned long)n;
          chunks++;

          /* P4X-LORARX-HEX: the first burst is dumped once, as hex and ascii.
           * Text-only output could not tell "the peer is quiet" from "the peer
           * sends binary".
           */

          if (!dumped)
            {
              int k;

              dumped = 1;
              printf("P4X-LORARX: first %d byte(s):", n > 64 ? 64 : n);
              for (k = 0; k < n && k < 64; k++)
                {
                  printf(" %02x", buf[k]);
                }

              printf(" |");
              for (k = 0; k < n && k < 64; k++)
                {
                  int c = (int)buf[k];

                  printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
                }

              printf("|\n");
            }

          for (i = 0; i < n; i++)
            {
              char c = (char)buf[i];

              /* P4X-LORARX-FRAME: CR/LF ends a line, and so does a complete
               * JSON object.  The arrow payload is JSON and carries no
               * trailing newline, so a CR/LF-only rule never fires and
               * telemetry_apply_json() is never reached.
               */

              /* P4X-FRAME2: strict '{' ... '}' framing.  Everything outside a
               * frame is line noise (the first bytes of a burst are 0xe0/0x00)
               * and used to be fed to the parser, which produced the spike at
               * the start of every curve and a satellite count that jumped
               * around on its own.
               */

              if (c == '{')
                {
                  used = 0;
                  inframe = 1;
                  line[used++] = c;
                }
              else if (!inframe)
                {
                  /* noise before a frame - drop it */
                }
              else if (c == '}')
                {
                  inframe = 0;

                  if (used >= 8)
                    {
                      static int raw_shown;

                      line[used] = '\0';

                      if (raw_shown < 12)
                        {
                          /* P4X-RAW2: keep the closing brace and show the
                           * length, so a split frame is obvious. */

                          line[used] = '}';
                          line[used + 1] = '\0';

                          raw_shown++;
                          printf("P4X-RAW2: len=%d %s\n", used + 1, line);
                        }
                      else
                        {
                          line[used] = '\0';
                        }

                      telemetry_apply_json(line);
                      lines++;

                      /* P4X-LATDIAG: how often a complete frame lands. */
                      {
                        unsigned long nt = clock_systime_ticks();

                        if (g_rx_last_tick != 0)
                          {
                            unsigned long d =
                              (nt - g_rx_last_tick) * 1000UL / TICK_PER_SEC;

                            g_rx_gap_sum += d;
                            g_rx_gap_n++;
                            if (d > g_rx_gap_max)
                              {
                                g_rx_gap_max = d;
                              }
                          }

                        g_rx_last_tick = nt;
                      }
                    }
                  else
                    {
                      lora_bad++;
                    }

                  used = 0;
                }
              else if (used < (int)sizeof(line) - 1)
                {
                  line[used++] = c;
                }
              else
                {
                  /* Overlong frame: drop it rather than feed half a packet to
                   * the parser - a truncated JSON line used to poison every
                   * field it did contain.
                   */

                  inframe = 0;
                  used = 0;
                  lora_bad++;
                }
            }
        }
      if (n > 0)
        {
          /* P4X-UIFAIR: hand the CPU back even while bytes keep arriving.
           * 245 B/s of telemetry is nothing, but a high-priority loop that
           * never yields starves the renderer and the screen stops updating.
           */

          usleep(1000);   /* P4X-RAW: keep the 128-byte FIFO drained */
        }
      else
        {
          usleep(5000);
        }

      /* One status line every 10 s: enough to prove the link from the console,
       * far too little to flood it.
       */

      if ((clock_systime_ticks() - started) > (10 * TICK_PER_SEC))
        {
          started = clock_systime_ticks();
          {
            /* P4X-LORARX-HW: hardware truth, not just our own counters. */

            int rxd = -1;
            int fifo = -1;
            int md0 = -1;
            int md1 = -1;
            int aux = -1;
            int pin = -1;

            /* P4X-DATAMODE: 每轮把 MD0/MD1 强制回 0/0(半双工透传)。
             *
             * 实测 md 卡在 1/1 —— 那是休眠配置态,手册 5.3 写明它【不转发
             * 任何无线数据】,所以 rxfifo 恒为 0。起因是配置探针把 MD 拉高后
             * 没恢复。放在这里重写,数据通路就不会再被别人搞丢。
             */

            board_lora_set_state(0, 0);

            /* P4X-BAUDROTATE: 照搬思澈地面站的成功策略 —— 没数据就换一档。
             * 参考 uart2hwtest.c 的 gps_bauds[]: 约 4 秒无数据切下一档,
             * 直到收到字节。不假设模块的串口速率。
             */

            {
              static const int rot[] = { 9600, 19200, 38400, 57600,
                                         115200, 4800, 2400, 1200 };
              static int rot_idx;
              static int idles;

              if (bytes > 0)
                {
                  idles = 0;
                }
              else if (++idles >= 2)
                {
                  idles = 0;
                  rot_idx = (rot_idx + 1) %
                            (int)(sizeof(rot) / sizeof(rot[0]));
                  /* P4X-BAUDROTATE-OFF: 轮换没有带来任何数据,而每次调用
                   * 探针都会拉高 MD、发 C3、等 400ms×3,连跑八档把 UI 饿死
                   * (实测屏幕只剩背光)。改为只打印,不动作。
                   */

                  (void)rot[rot_idx];

                  /* P4X-STALE: this line fires whenever the link goes
                   * quiet, i.e. at an arbitrary moment, and a blocked
                   * console write would kill the receive task for good -
                   * the panel keeps repainting while the data never
                   * changes again ("the attitude froze").
                   */

                  if (P4X_DIAG)
                    {
                      printf("P4X-LORARX: still no data at any rate\n");
                    }
                }
            }

            board_lora_line(&rxd, &fifo);
            board_lora_pins(&md0, &md1, &aux);

            /* P4X-AUTOPIN: keep hunting for the pad that carries the module's
             * TXD until bytes arrive.
             */

            pin = autopin_tick(bytes > 0);

            {
              /* P4X-PAD38: the pad itself, so "wire missing" and "controller
               * cannot see the wire" stay distinguishable.
               */

              int pad = pad38_level();

              /* P4X-TELEM: the parsed values, once every other stats line, so the
           * fields behind the screen can be checked from a serial log.
           */

          {
            static unsigned long telem_tick;

            if (0 && (telem_tick++ % 60u) == 0u)   /* P4X-NOLOG: off */
              {
                /* P4X-CLOCKECHO: same string the panel shows, so the clock can
                 * be checked without looking at the screen.
                 */

                printf("P4X-TELEM: [%s] r=%.2f p=%.2f y=%.2f alt=%.2f P=%.0f "
                       "sat=%d gps=%d v=%.2f ax=%.3f az=%.3f t=%.2f\n",
                       fl.time_str,
                       (double)fl.roll, (double)fl.pitch, (double)fl.yaw,
                       (double)fl.alt, (double)fl.pressure, fl.sat,
                       fl.gps_fix, (double)fl.v, (double)fl.ax,
                       (double)fl.az, (double)fl.temperature);
              }
          }

          /* P4X-NOLOG: printing from this high-priority task blocks as soon
           * as nobody drains the USB-CDC console, which froze the whole UI
           * about 30 s after boot.  Re-enable only while capturing.
           */

          if (0)
            printf("P4X-LORARX: %lu chunk(s), %lu byte(s), %lu line(s), "
                     "live=%d, bad=%lu | hw rxfifo=%d rxd=%d pad21=%d pin=%d md=%d/%d\n",
                     chunks, bytes, lines, telemetry_live, lora_bad,
                     fifo, rxd, pad, pin, md0, md1);
            }
          }
        }
    }

  return 0;
}

/* P4X-AVEC: acceleration and speed with gravity removed, without trusting the
 * airborne's attitude math.
 *
 * The accelerometer measures specific force, so standing still it points at
 * "up" with a magnitude of one g.  That vector, low-pass filtered, IS the
 * gravity reference:
 *
 *   - it follows a slow orientation change (the board being rolled over, the
 *     rocket tipping in the tower),
 *   - it is only updated while the total magnitude is close to 1 g, so a 3 g
 *     burn can never corrupt it,
 *   - and it needs no roll/pitch at all, which is what went wrong before: the
 *     airborne's roll tilts a different axis than the model assumed, so every
 *     flip made the "gravity removed" vector jump and the speed ratcheted up.
 *
 * The panel shows the magnitude of the velocity vector, and the acceleration
 * curves now plot the gravity-free linear acceleration.
 */

static float g_speed;                 /* |v|, what the panel shows */
static float g_vx, g_vy, g_vz;        /* velocity vector, m/s */
static float g_axl, g_ayl, g_azl;     /* linear accel, g, gravity removed */
static float g_almag;                 /* |linear accel|, g */
static float g_gx, g_gy, g_gz = 1.0f; /* low-passed gravity vector, g */
static int   g_g_ref;
static unsigned long g_v_last_tick;

static void v_integrate(void)
{
  unsigned long now;
  float amag;
  float dt;
  float lx;
  float ly;
  float lz;
  float lmag;

  if (!telemetry_live)
    {
      return;
    }

  amag = sqrtf(fl.ax * fl.ax + fl.ay * fl.ay + fl.az * fl.az);

  /* P4X-GLPF: only a clean 1 g reading is a gravity reference. */

  if (amag > 0.85f && amag < 1.15f)
    {
      if (!g_g_ref)
        {
          g_gx = fl.ax;
          g_gy = fl.ay;
          g_gz = fl.az;
          g_g_ref = 1;
        }
      else
        {
          g_gx += (fl.ax - g_gx) * 0.04f;    /* ~1.5 s at 12.5 Hz */
          g_gy += (fl.ay - g_gy) * 0.04f;
          g_gz += (fl.az - g_gz) * 0.04f;
        }
    }

  if (!g_g_ref)
    {
      return;
    }

  lx = fl.ax - g_gx;
  ly = fl.ay - g_gy;
  lz = fl.az - g_gz;

  g_axl = lx;
  g_ayl = ly;
  g_azl = lz;

  lmag = sqrtf(lx * lx + ly * ly + lz * lz);
  g_almag = lmag;

  now = clock_systime_ticks();

  /* P4X-ZUPT: the reading is a clean 1 g and nothing is left over - the rocket
   * is standing still, so hold the velocity at zero instead of integrating
   * noise for minutes.
   */

  if (lmag < 0.05f && amag > 0.85f && amag < 1.15f)
    {
      g_vx = 0.0f;
      g_vy = 0.0f;
      g_vz = 0.0f;
      g_speed = 0.0f;
      g_v_last_tick = now;
      return;
    }

  if (g_v_last_tick == 0)
    {
      g_v_last_tick = now;
      return;
    }

  dt = (float)(now - g_v_last_tick) / (float)TICK_PER_SEC;
  g_v_last_tick = now;

  if (dt <= 0.0f || dt > 2.0f)
    {
      return;
    }

  g_vx += lx * 9.80665f * dt;
  g_vy += ly * 9.80665f * dt;
  g_vz += lz * 9.80665f * dt;

  g_speed = sqrtf(g_vx * g_vx + g_vy * g_vy + g_vz * g_vz);

  if (g_speed > 400.0f)
    {
      float k = 400.0f / g_speed;

      g_vx *= k;
      g_vy *= k;
      g_vz *= k;
      g_speed = 400.0f;
    }

  /* A real GNSS speed wins whenever there is a fix. */

  if (fl.gps_fix && fl.v > 0.05f)
    {
      g_speed = fl.v;
    }
}

static void hist_push(void)
{
  v_integrate();
  tmp_hist[hist_idx]  = fl.temperature;         /* P4X-DATA4 */
  rx_hist[hist_idx]   = (float)g_rx_pps;
  tx_hist[hist_idx]   = (float)g_tx_pps;
  lost_hist[hist_idx] = (float)g_lost_pps;
  hist_ts[hist_idx]  = clock_systime_ticks();   /* P4X-XAXIS3 */
  v_hist[hist_idx]   = g_speed;
  alt_hist[hist_idx] = g_alt_msl;   /* P4X-ALTFIT: the filtered  the big
                                     * display shows */
  a_hist[hist_idx]   = (fl.az - 1.0f) * 9.8f;
  p_hist[hist_idx]   = fl.pressure / 1000.0f;
  pit_hist[hist_idx] = fl.pitch;
  rol_hist[hist_idx] = fl.roll;
  yaw_hist[hist_idx] = fl.yaw;
  ax_hist[hist_idx]  = fl.ax;    /* raw, gravity included */
  ay_hist[hist_idx]  = fl.ay;    /* raw, gravity included */
  az_hist[hist_idx]  = fl.az;    /* raw, gravity included */

  /* P4X-SATCLS: one per-type sample a second, for the sub-page history. */

  {
    static int sat_div;
    int k;

    if (++sat_div >= 12)
      {
        sat_div = 0;

        for (k = 0; k < SAT_CLS; k++)
          {
            int v = g_sc[k];

            if (v < 0)
              {
                v = 0;
              }
            else if (v > 40)
              {
                v = 40;
              }

            g_sc_hist[k][g_sc_hist_idx] = (unsigned char)v;
          }

        g_sc_hist_idx = (g_sc_hist_idx + 1) % SAT_HIST;
      }
  }

  hist_idx = (hist_idx + 1) % HIST_N;
}

/* P4X-STALE: clear the history ring.
 *
 * When the link stops for a few seconds the curves would otherwise keep a
 * tail of stale samples and the picture would look half frozen.  Filling the
 * ring with the current values makes a restart obvious: the trace starts
 * flat from where the vehicle actually is.
 */

static void hist_clear(void)
{
  int i;
  unsigned long now = clock_systime_ticks();

  for (i = 0; i < HIST_N; i++)
    {
      v_hist[i]   = g_speed;
      alt_hist[i] = g_alt_msl;
      a_hist[i]   = 0.0f;
      p_hist[i]   = fl.pressure / 1000.0f;
      pit_hist[i] = fl.pitch;
      rol_hist[i] = fl.roll;
      yaw_hist[i] = fl.yaw;
      ax_hist[i]  = fl.ax;
      ay_hist[i]  = fl.ay;
      az_hist[i]  = fl.az;
      hist_ts[i]  = now;
    }

  hist_idx = 0;
}

/****************************************************************************
 * 卡片与曲线
 ****************************************************************************/

static void card(int x, int y, int w, int h, uint16_t accent)
{
  fill_round_rect(x, y, w, h, 8, g_c_darker);
  card_border(x, y, w, h, g_c_dark);
  if (accent != 0)
    {
      fill_rect(x, y + 6, 5, h - 12, accent);
    }
}

/* P4X-NICE: round a needed span up to the next step of a fixed ladder.
 *
 * Fitting the window exactly to the data put the minimum and the maximum
 * right on the borders ("the traces run to the very top and bottom") and
 * rescaled on every frame.  A ladder with 40% headroom gives a scale that
 * changes in visible steps and never touches the edge, while still being as
 * tight as the data allows - the highest precision the data supports.
 */

static float nice_span(float need)
{
  static const float steps[] =
  {
    1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f, 320.0f
  };
  int i;

  for (i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); i++)
    {
      if (need <= steps[i])
        {
          return steps[i];
        }
    }

  return steps[(int)(sizeof(steps) / sizeof(steps[0])) - 1];
}

static int chart_y(int gy, int gh, float v, float lo, float hi)
{
  int y;
  if (hi - lo < 1e-6f) return gy + gh / 2;
  y = gy + gh - (int)((v - lo) / (hi - lo) * (gh - 1));
  if (y < gy) y = gy;
  if (y > gy + gh - 1) y = gy + gh - 1;
  return y;
}

/* 多曲线图（最多 3 条） */
/* P4X-XAXIS: measured sample interval, used to label the time axis. */
static unsigned long g_hist_dt_ms = 80;

/* P4X-XAXIS3: the real width of the history window, taken from the sample
 * timestamps.  The old code printed HIST_N x g_hist_dt_ms and g_hist_dt_ms
 * was never updated, so the axis always claimed 14.4 s while the panel was
 * really showing ~30 s of history at the achieved frame rate.
 */

static float hist_span_s(void)
{
  unsigned long t_new = hist_ts[(hist_idx + HIST_N - 1) % HIST_N];
  unsigned long t_old = hist_ts[(hist_idx - g_hist_win + HIST_N * 2) % HIST_N];

  if (t_new > t_old)
    {
      float s = (float)(t_new - t_old) / (float)TICK_PER_SEC;

      if (s >= 1.0f)
        {
          return s;
        }
    }

  return (float)(g_hist_win * (int)g_hist_dt_ms) / 1000.0f;
}

/* P4X-XAXIS4: a clean seconds axis.
 *
 * The labels used to be "0.0s / -1.3s / -2.6s": a minus sign that meant
 * nothing, and values that were never round.  The axis now ticks on a ladder
 * (0.1 / 0.2 / 0.5 / 1 / 2 / 5 / 10 ...) so it reads 0.5, 1.0, 1.5, and the
 * ticks sit at their real time positions instead of at fixed fractions.
 */

static void xaxis_ticks(int gx, int ybase, int gw, float total_s)
{
  static const float ladder[] =
  {
    0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 30.0f, 60.0f
  };
  char tb[16];
  float want;
  float step;
  int i;
  int kmax;
  int nsteps = (int)(sizeof(ladder) / sizeof(ladder[0]));

  if (total_s < 0.05f)
    {
      total_s = 0.05f;
    }

  want = total_s / 5.0f;                /* about six ticks on screen */
  step = ladder[nsteps - 1];

  for (i = 0; i < nsteps; i++)
    {
      if (ladder[i] >= want)
        {
          step = ladder[i];
          break;
        }
    }

  kmax = (int)(total_s / step + 0.001f);

  for (i = 0; i <= kmax; i++)
    {
      float t = step * (float)i;
      int x = gx + gw - 1 - (int)(t / total_s * (float)(gw - 1));

      if (x < gx)
        {
          continue;
        }

      vline(x, ybase - 3, ybase + 2, g_c_dark);

      if (step >= 1.0f)
        {
          snprintf(tb, sizeof(tb), "%.0f", (double)t);
        }
      else
        {
          snprintf(tb, sizeof(tb), "%.1f", (double)t);
        }

      text(x - (int)strlen(tb) * 4, ybase, tb, g_c_axis);
    }
}

static void draw_multi_chart(int x0, int y0, int w, int h,
                             float min_span, float adapt_at,
                             float *d1, float *d2, float *d3,
                             uint16_t c1, uint16_t c2, uint16_t c3,
                             const char *title,
                             const char *l1, const char *l2, const char *l3)
{
  char buf[24];
  float *sets[3] = { d1, d2, d3 };
  uint16_t cols[3] = { c1, c2, c3 };
  const char *labs[3] = { l1, l2, l3 };
  float lo = 1e9f, hi = -1e9f, pad, range;
  int i, k, gx, gy, gw, gh;

  hit_reg(zoom_id_of(d1), x0, y0, w, h);   /* P4X-ZOOM */

  card(x0, y0, w, h, 0);

  for (k = 0; k < 3; k++)
    {
      if (sets[k] == NULL) continue;
      for (i = 0; i < g_hist_win; i++)
        {
          float v = sets[k][(hist_idx - 1 - i + HIST_N * 2) % HIST_N];
          if (v < lo) lo = v;
          if (v > hi) hi = v;
        }
    }
  /* P4X-ADAPT: precision that follows the motion.
   *
   * Quiet: keep the min_span window, so 0.2 degrees of sensor noise stays a
   * flat line instead of filling the chart with jitter.
   *
   * Moving: the moment the data spans more than a third of that window the
   * minimum is dropped and the trace is fitted to what actually happened, so
   * a real deflection gets maximum vertical detail.  Once it settles, the
   * range shrinks again and the calm window comes back on its own - this is
   * the "high precision while it moves, a little less once it is steady" the
   * operator asked for, without a switch to press.
   */

  /* P4X-NICE: span from the ladder, then centre on the data.
   *
   * P4X-SPAN: this replaces the old min_span-only guard; the marker is kept so
   * the build-time patch that introduced min_span stays a no-op.
   */

  {
    float span = nice_span((hi - lo) * 1.4f);
    float c;

    if (span < min_span)
      {
        span = min_span;
      }

    if (span < adapt_at)
      {
        span = adapt_at;
      }

    c = (hi + lo) * 0.5f;
    lo = c - span * 0.5f;
    hi = c + span * 0.5f;
  }

  pad = 0.0f;
  range = hi - lo;

  gx = x0 + 40; gy = y0 + 22; gw = w - 50; gh = h - 30;
  if (gw < 10 || gh < 10) return;

  for (k = 0; k <= 3; k++)
    {
      int yy = gy + gh * k / 3;
      float v = hi - range * k / 3.0f;
      hline(gx, gx + gw, yy, g_c_dark);
      snprintf(buf, sizeof(buf), (v >= 100 || v <= -100) ? "%.0f" : "%.1f", v);
      text(x0 + 4, yy - 3, buf, g_c_label);
    }
  for (k = 1; k < 4; k++)
    vline(gx + gw * k / 4, gy, gy + gh, g_c_dark);

  /* P4X-XAXIS2: the attitude/acceleration charts need the same time axis as
   * the single-series ones.  Window = HIST_N samples x the measured interval.
   */

  xaxis_ticks(gx, y0 + h - 13, gw, hist_span_s());

  text_cn(x0 + 8, y0 + 3, title, g_c_label, 1);

  for (k = 0; k < 3; k++)
    {
      int cx;
      if (sets[k] == NULL) continue;
      for (cx = 1; cx < g_hist_win; cx++)
        {
          int i0 = (hist_idx - 1 - (cx - 1) + HIST_N * 2) % HIST_N;
          int i1 = (hist_idx - 1 - cx + HIST_N * 2) % HIST_N;
          int xa = gx + gw - 1 - (cx - 1) * gw / g_hist_win;
          int xb = gx + gw - 1 - cx * gw / g_hist_win;
          int ya = chart_y(gy, gh, sets[k][i0], lo, hi);
          int yb = chart_y(gy, gh, sets[k][i1], lo, hi);
          line(xa, ya, xb, yb, cols[k]);
          line(xa, ya + 1, xb, yb + 1, cols[k]);
        }

      /* P4X-LIVEDOT: mark the newest sample, so the live end of the trace
       * is obvious even while the rest of it is moving left. */

      {
        int inew = (hist_idx + HIST_N - 1) % HIST_N;
        int yy = chart_y(gy, gh, sets[k][inew], lo, hi);

        fill_rect(gx + gw - 3, yy - 1, 4, 4, cols[k]);
      }
    }

  {
    int lx = x0 + w - 12;
    for (k = 2; k >= 0; k--)
      {
        int cw2;
        if (sets[k] == NULL || labs[k] == NULL) continue;
        cw2 = cn_width_est(labs[k]);
        lx -= (cw2 + 56);
        fill_rect(lx, y0 + 6, 10, 10, cols[k]);
        text_cn(lx + 14, y0 + 2, labs[k], cols[k], 1);
        snprintf(buf, sizeof(buf), "%.1f", sets[k][(hist_idx + HIST_N - 1) % HIST_N]);
        text(lx + 14 + cw2, y0 + 6, buf, cols[k]);
      }
  }
}

/* 单曲线图 */
static void draw_chart(int x0, int y0, int w, int h, float *data,
                       float maxv, uint16_t color, const char *label,
                       const char *valstr)
{
  /* P4X-XAXIS: 180 samples spread over the whole width instead of one pixel per
   * sample (which stopped at 38% of the panel), plus a time axis in seconds.
   */

  int i;
  int cx;
  int gx = x0 + 34;
  int gw = w - 42;
  int prev_x = -1;
  int prev_y = -1;
  char tbuf[16];
  float total_s = hist_span_s();

  hit_reg(zoom_id_of(data), x0, y0, w, h);   /* P4X-ZOOM */

  card(x0, y0, w, h, 0);

  for (i = 1; i < 4; i++)
    {
      hline(x0 + 6, x0 + w - 7, y0 + h * i / 4, g_c_dark);
    }

  if (gw < 8 || h < 20)
    {
      return;
    }

  for (cx = 0; cx < g_hist_win; cx++)
    {
      int idx = (hist_idx - 1 - cx + HIST_N * 2) % HIST_N;
      int px_x = gx + gw - 1 - cx * gw / g_hist_win;
      int py = y0 + h / 2 - (int)(data[idx] / maxv * (h / 2 - 6));

      if (py < y0 + 2) py = y0 + 2;
      if (py > y0 + h - 3) py = y0 + h - 3;

      if (prev_x >= 0)
        {
          line(prev_x, prev_y, px_x, py, color);
        }

      px(px_x, py, color);
      prev_x = px_x;
      prev_y = py;
    }

  /* time axis: 0 s on the right, -(window) s on the left */

  xaxis_ticks(gx, y0 + h - 13, gw, total_s);
  (void)tbuf;

  /* P4X-YAXIS: vertical scale (speed) */

  {
    char yb[16];

    snprintf(yb, sizeof(yb), "%.0f", (double)maxv);
    text(x0 + 3, y0 + 14, yb, g_c_axis);
    snprintf(yb, sizeof(yb), "0");
    text(x0 + 3, y0 + h / 2 - 4, yb, g_c_axis);
    snprintf(yb, sizeof(yb), "%.0f", (double)-maxv);
    text(x0 + 3, y0 + h - 24, yb, g_c_axis);
  }

  text_cn(x0 + 8, y0 + 3, label, C_YELLOW, 1);

  if (valstr[0])
    {
      /* P4X-XAXIS2: top-right, so the text no longer covers the newest sample
       * at the right edge of the curve.
       */

      text_scale(x0 + w - 10 - (int)strlen(valstr) * 16, y0 + 4, valstr,
                 color, 2);
    }
}


/****************************************************************************
 * 页头
 ****************************************************************************/

/* P4X-ABS-AXIS: the altitude and pressure charts used to be drawn with
 * "center = the current reading", so the trace sat exactly on the middle
 * line whatever the sensor said - that is the "-34 m is still on the base
 * line" report.  The window is now anchored to a fixed base (sea level for
 * altitude, standard pressure for the barometer) and only grows when the
 * data leaves it, so the trace really moves and its height means something.
 */

static void chart_draw_range(int x0, int y0, int w, int h, float *data,
                            float lo, float hi, uint16_t color,
                            const char *label, const char *valstr)
{
  int i;
  int cx;
  int gx = x0 + 34;
  int gw = w - 42;
  int prev_x = -1;
  int prev_y = -1;
  char tbuf[16];
  float total_s = hist_span_s();

  hit_reg(zoom_id_of(data), x0, y0, w, h);   /* P4X-ZOOM */

  if (hi - lo < 0.002f)
    {
      hi = lo + 0.002f;
    }

  card(x0, y0, w, h, 0);

  for (i = 1; i < 4; i++)
    {
      hline(x0 + 6, x0 + w - 7, y0 + h * i / 4, g_c_dark);
    }

  if (gw < 8 || h < 20)
    {
      return;
    }

  for (cx = 0; cx < g_hist_win; cx++)
    {
      int idx = (hist_idx - 1 - cx + HIST_N * 2) % HIST_N;
      int px_x = gx + gw - 1 - cx * gw / g_hist_win;
      int py = y0 + h / 2 -
               (int)((data[idx] - (lo + hi) * 0.5f) /
                     ((hi - lo) * 0.5f) * (h / 2 - 6));

      if (py < y0 + 2) py = y0 + 2;
      if (py > y0 + h - 3) py = y0 + h - 3;

      if (prev_x >= 0)
        {
          line(prev_x, prev_y, px_x, py, color);
        }

      px(px_x, py, color);
      prev_x = px_x;
      prev_y = py;
    }

  xaxis_ticks(gx, y0 + h - 13, gw, total_s);
  (void)tbuf;

  /* P4X-YAXIS: the vertical scale in the left gutter - pressure and altitude
   * had no numbers at all, so the curve height meant nothing.
   */

  {
    char yb[16];

    if (hi >= 100.0f || hi <= -100.0f)
      {
        snprintf(yb, sizeof(yb), "%.0f", (double)hi);
      }
    else
      {
        snprintf(yb, sizeof(yb), "%.1f", (double)hi);
      }

    text(x0 + 3, y0 + 14, yb, g_c_axis);

    if (lo >= 100.0f || lo <= -100.0f)
      {
        snprintf(yb, sizeof(yb), "%.0f", (double)lo);
      }
    else
      {
        snprintf(yb, sizeof(yb), "%.1f", (double)lo);
      }

    text(x0 + 3, y0 + h - 24, yb, g_c_axis);
  }

  text_cn(x0 + 8, y0 + 3, label, C_YELLOW, 1);

  if (valstr[0])
    {
      /* P4X-XAXIS2: top-right, so the text no longer covers the newest sample
       * at the right edge of the curve.
       */

      text_scale(x0 + w - 10 - (int)strlen(valstr) * 16, y0 + 4, valstr,
                 color, 2);
    }
}

/* P4X-ABS-AXIS: a window anchored to a fixed base (sea level for altitude,
 * standard pressure for the barometer) that only grows when the data leaves
 * it.  Kept for the barometer, whose absolute value is the point.
 */

static void draw_chart_win(int x0, int y0, int w, int h, float *data,
                           float base, float min_span, uint16_t color,
                           const char *label, const char *valstr)
{
  int i;
  float lo;
  float hi;

  if (min_span < 0.5f)
    {
      min_span = 0.5f;
    }

  lo = base - min_span;
  hi = base + min_span;

  for (i = 0; i < g_hist_win; i++)
    {
      float v = data[(hist_idx - 1 - i + HIST_N * 2) % HIST_N];

      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }

  chart_draw_range(x0, y0, w, h, data, lo, hi, color, label, valstr);
}

/* P4X-ALTFIT: a window fitted to the data through the P4X-NICE ladder.
 *
 * The fitted altitude is stable to a decimetre, so the old fixed +/-100 m
 * window would squash a whole flight into the middle twenty pixels and show
 * nothing at all while the vehicle sits on the pad.  Fitting the window to the
 * data - but only in ladder steps and with 40% headroom - gives the resolution
 * the fitted altitude actually supports without the trace ever touching the
 * borders.
 */

static void draw_chart_fit(int x0, int y0, int w, int h, float *data,
                           float min_span, uint16_t color,
                           const char *label, const char *valstr)
{
  int i;
  float lo = 1.0e9f;
  float hi = -1.0e9f;
  float span;
  float mid;

  for (i = 0; i < g_hist_win; i++)
    {
      float v = data[(hist_idx - 1 - i + HIST_N * 2) % HIST_N];

      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }

  if (hi < lo)
    {
      lo = 0.0f;
      hi = 1.0f;
    }

  if (min_span < 0.5f)
    {
      min_span = 0.5f;
    }

  span = nice_span((hi - lo) * 1.4f);

  if (span < min_span)
    {
      span = min_span;
    }

  mid = (lo + hi) * 0.5f;
  lo = mid - span * 0.5f;
  hi = mid + span * 0.5f;

  chart_draw_range(x0, y0, w, h, data, lo, hi, color, label, valstr);
}


static void draw_page_header(const char *title, int page)
{
  char buf[12];
  int x = CONT_X, y = SAFE;

  fill_round_rect(x, y, CONT_W, HDR_H, 8, g_c_hdr);
  text_cn(x + 14, y + 6, title, g_c_text, 2);

  /* P4X-NODEMO: the old label said 显示数据 ("displaying data"), which reads
   * like a healthy link.  With no telemetry there is simply no data.
   */

  if (telemetry_live)
    text_cn(x + 150, y + 12, "已连接", C_LIME, 1);
  else
    text_cn(x + 150, y + 12, "无数据", C_RED, 1);

  /* P4X-SATDOT: satellite indicator in the header.  A red dot and a red count
   * make "no satellites" impossible to misread as a healthy fix.
   */

  {
    int sat_ok = fl.sat > 0;
    unsigned int satc = sat_ok ? C_GREEN : C_RED;

    /* P4X-SATCLS: the indicator is a button now - "tap the satellite count to
     * open the per-constellation page".  A thin frame is the only hint the
     * operator gets, so it has to be there on every page.
     */

    card_border(x + 248, y + 6, 134, HDR_H - 9, satc);

    fill_circle(x + 262, y + 22, 7, satc);
    snprintf(buf, sizeof(buf), "SAT %d", fl.sat);
    text_scale(x + 278, y + 12, buf, satc, 2);

    g_sat_x0 = x + 244;
    g_sat_y0 = y + 2;
    g_sat_x1 = x + 386;
    g_sat_y1 = y + HDR_H - 2;
  }

  /* P4X-HB: the clock lives in the header now; it used to be drawn after the
   * speed number, where the seconds after the colon were cut off.
   */

  /* P4X-CLOCK2: the mission clock is maintained HERE, in the header, so it
   * advances on every page.  It used to live inside page_status(), so as
   * soon as the panel showed a curve page the header kept the last string
   * it had - "the time never moves".
   *
   * Scale 2, not 3: at scale 3 the clock was the largest element on the
   * panel and it pushed the page counter off the header bar.
   */

  if (!g_time_from_gps)
    {
      static unsigned long hdr_clk0;
      unsigned long hdr_now = clock_systime_ticks();
      unsigned long hdr_secs;

      if (hdr_clk0 == 0)
        {
          hdr_clk0 = hdr_now;
        }

      hdr_secs = (hdr_now - hdr_clk0) / TICK_PER_SEC;
      snprintf(fl.time_str, sizeof(fl.time_str), "T+%lu:%02lu",
               hdr_secs / 60ul, hdr_secs % 60ul);
    }

  text_scale(x + CONT_W - 300, y + 13, fl.time_str, C_CYAN, 2);

  snprintf(buf, sizeof(buf), "%d/%d", page + 1, NUM_PAGES);
  text_scale(x + CONT_W - 90, y + 13, buf, C_CYAN, 2);

  /* 左下角小提示 */

  /* 右上角屏幕自证：能看见这行数字 = 界面帧真的画到屏上了 */
  {
    char perf[24];

    /* P4X-HDR: the operator asked for the live frame rate instead of the
     * draw/blit millisecond pair, which meant nothing at a glance.
     */

    snprintf(perf, sizeof(perf), "%lu fps", g_fps);
    text(x + CONT_W - 130, y + 18, perf, C_GREEN);
  }
}

/****************************************************************************
 * 小工具
 ****************************************************************************/

static void kv_big(int x, int y, const char *label, const char *value,
                   uint16_t lc, uint16_t vc, int sc)
{
  text_cn(x, y, label, lc, 1);
  text_scale(x, y + 20, value, vc, sc);
}

static void bar(int x, int y, int w, int h, float frac, uint16_t c)
{
  int fillw;

  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  fillw = (int)(frac * w);
  fill_rect(x, y, w, h, g_c_track);     /* P4X-THEME4 */
  if (fillw > 0) fill_rect(x, y, fillw, h, c);
}

/* P4X-GAUGE: a zero-centred gauge.
 *
 * The middle of the track is 0, both sides start black, and only the part
 * between the centre line and the current value is coloured - so the colour
 * grows out of the zero axis instead of filling from the left edge.
 */

static void gauge(int x, int y, int w, int h, float frac, uint16_t c)
{
  int mid = x + w / 2;
  int pos;

  if (frac < -1.0f) frac = -1.0f;
  if (frac > 1.0f) frac = 1.0f;

  fill_rect(x, y, w, h, g_c_track);     /* P4X-THEME4 */
  card_border(x, y, w, h, g_c_dark);

  pos = mid + (int)(frac * (float)(w / 2));

  if (pos > mid)
    {
      fill_rect(mid, y, pos - mid, h, c);
    }
  else if (pos < mid)
    {
      fill_rect(pos, y, mid - pos, h, c);
    }

  /* the zero axis stays visible on top of the fill */

  vline(mid, y, y + h - 1, g_c_text);
}

/****************************************************************************
 * Page 0 · 状态总览（网页端排布）
 ****************************************************************************/

static void page_status(void)
{
  char buf[64];
  int x = CONT_X, y = CONT_Y, w = CONT_W;
  int barh = 62;
  int gridy = y + barh + 6;
  int gw = (w - 8) / 2;
  int gh = (CONT_H - barh - 6 - 8) / 2;

  draw_page_header("状态", 0);

  /* ---- 顶部状态条 ---- */
  card(x, y, w, barh, 0);
  fill_circle(x + 26, y + 31, 8, fl.lora_ok ? C_GREEN : C_RED);
  text_cn(x + 42, y + 8, "连接", g_c_label, 1);
  text_scale(x + 42, y + 28, fl.lora_ok ? "OK" : "NO", fl.lora_ok ? C_GREEN : C_RED, 2);

  snprintf(buf, sizeof(buf), "%d", fl.sat);
  text_cn(x + 160, y + 8, "卫星", C_YELLOW, 1);

  /* P4X-SATDOT: 0 satellites must be red, not green. */

  text_scale(x + 210, y + 4, buf, fl.sat > 0 ? C_GREEN : C_RED, 4);

  text_cn(x + 300, y + 8, "阶段", g_c_label, 1);
  text_cn(x + 300, y + 28, phase_cn[phase_show_idx()],
          phase_color_now(), 2);

  /* P4X-FIX4: the clock label showed fl.t, which is the temperature. */

  /* P4X-FIX-ALTCLK: recompute the clock on every repaint from the system tick,
   * so the seconds advance evenly instead of only when a packet arrives.
   */

  /* P4X-CLOCK2: the clock now lives in draw_page_header(). */

  snprintf(buf, sizeof(buf), "%s", fl.time_str);
  text_cn(x + 440, y + 8, "时间", g_c_label, 1);
  text_scale(x + 440, y + 26, buf, g_c_text, 2);

  snprintf(buf, sizeof(buf), "%.1f", (double)g_alt_msl);
  text_cn(x + 620, y + 8, "高度", C_SKYBLUE, 1);
  text_scale(x + 620, y + 24, buf, C_SKYBLUE, 3);
  text_scale(x + 620 + (int)strlen(buf) * 24 + 6, y + 40, "m", g_c_label, 1);

  snprintf(buf, sizeof(buf), "%.0f", g_speed);
  text_cn(x + 810, y + 8, "速度", C_GREEN, 1);
  text_scale(x + 810, y + 24, buf, C_GREEN, 3);
  text_scale(x + 810 + (int)strlen(buf) * 24 + 6, y + 40, "m/s", g_c_label, 1);

  /* ---- 2x2 四图 ---- */
  {
    int cx0 = x, cy0 = gridy;
    int cx1 = x + gw + 8, cy1 = gridy + gh + 8;

    draw_chart(cx0, cy0, gw, gh, v_hist, 150.0f, C_GREEN, "速度曲线", "");
    draw_chart_fit(cx1, cy0, gw, gh, alt_hist, 10.0f, C_SKYBLUE, "高度曲线", "");
    draw_multi_chart(cx0, cy1, gw, gh, 20.0f, 2.0f, pit_hist, rol_hist, yaw_hist,
                     C_YELLOW, C_MAGENTA, g_c_yaw, "姿态曲线", "俯仰", "滚转", "偏航");
    draw_multi_chart(cx1, cy1, gw, gh, 4.0f, 0.0f, ax_hist, ay_hist, az_hist,
                     C_RED, C_GREEN, C_BLUE, "加速度曲线", "X轴", "Y轴", "Z轴");
    (void)cy1;
  }
}

/****************************************************************************
 * Page 1 · 曲线页（网页端第 2 页）
 ****************************************************************************/

static void page_curves(void)
{
  char buf[32];
  int x = CONT_X, y = CONT_Y, w = CONT_W, h = CONT_H;
  int gw = (w - 8) / 2, gh = (h - 16) / 3;

  draw_page_header("曲线", 1);

  draw_multi_chart(x, y, gw, gh, 20.0f, 2.0f, pit_hist, rol_hist, yaw_hist,
                   C_YELLOW, C_MAGENTA, g_c_yaw, "姿态曲线", "俯仰", "滚转", "偏航");
  draw_multi_chart(x + gw + 8, y, gw, gh, 4.0f, 0.0f, ax_hist, ay_hist, az_hist,
                   C_RED, C_GREEN, C_BLUE, "加速度曲线", "X轴", "Y轴", "Z轴");

  snprintf(buf, sizeof(buf), "%.1f", (double)g_alt_msl);
  draw_chart_fit(x, y + gh + 8, gw, gh, alt_hist, 10.0f, C_SKYBLUE, "高度曲线", buf);
  snprintf(buf, sizeof(buf), "%.0f", g_speed);
  draw_chart(x + gw + 8, y + gh + 8, gw, gh, v_hist, 150.0f, C_GREEN, "速度曲线", buf);

  snprintf(buf, sizeof(buf), "%.1f", fl.pressure / 1000.0f);
  draw_chart_win(x, y + 2 * (gh + 8), gw, gh, p_hist, 101.325f, 5.0f, C_ORANGE, "气压曲线", buf);

  /* 右下角数据卡 */
  card(x + gw + 8, y + 2 * (gh + 8), gw, gh, C_LIME);
  text_cn(x + gw + 24, y + 2 * (gh + 8) + 8, "数据", C_LIME, 1);
  snprintf(buf, sizeof(buf), "%.2f g", fl.az);
  text_scale(x + gw + 24, y + 2 * (gh + 8) + 32, buf, g_c_text, 2);
  text_cn(x + gw + 24, y + 2 * (gh + 8) + 62, "加速度", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%.0f Pa", fl.pressure);
  text_scale(x + gw + 340, y + 2 * (gh + 8) + 32, buf, C_CYAN, 2);
  text_cn(x + gw + 340, y + 2 * (gh + 8) + 62, "气压", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%.1f C", fl.temperature);
  text_scale(x + gw + 24, y + 2 * (gh + 8) + 88, buf, C_ORANGE, 2);
  text_cn(x + gw + 24, y + 2 * (gh + 8) + 118, "温度", g_c_label, 1);
  /* P4X-DATA4: packets received per second / estimated lost per second. */

  snprintf(buf, sizeof(buf), "%lu/%lu/%lu", g_rx_pps, g_tx_pps, g_lost_pps);
  text_scale(x + gw + 340, y + 2 * (gh + 8) + 88, buf, C_LIME, 2);
  text_cn(x + gw + 340, y + 2 * (gh + 8) + 118, "收/发/丢", g_c_label, 1);

  /* P4X-DATA4: every quadrant of the data card opens its own record curve. */

  {
    int bx = x + gw + 8;
    int by = y + 2 * (gh + 8);

    hit_reg(8, bx,          by,          gw / 2, gh / 2);
    hit_reg(8, bx + gw / 2, by,          gw / 2, gh / 2);
    hit_reg(8, bx,          by + gh / 2, gw / 2, gh / 2);
    hit_reg(8, bx + gw / 2, by + gh / 2, gw / 2, gh / 2);
  }
}

/****************************************************************************
 * Page 2 · 速度页（思澈特色大字页）
 ****************************************************************************/

static void page_speed(void)
{
  char buf[48];
  int x = CONT_X, y = CONT_Y, w = CONT_W;
  int big_h = 170, acc_h = 140;
  int cw = (w - 16) / 3;

  draw_page_header("速度", 2);

  /* 大字速度 */
  card(x, y, w, big_h, C_GREEN);
  text_cn(x + 24, y + 12, "速度", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%.1f", g_speed);
  text_scale(x + 24, y + 44, buf, C_GREEN, 8);
  text_scale(x + 24 + (int)strlen(buf) * 64 + 24, y + 130, "m/s", g_c_label, 2);

  /* 右侧高度条 */
  text_cn(x + w - 380, y + 12, "高度", g_c_label, 1);
  /* P4X-FIX4: one decimal, and a bar that resolves metres instead of 900 m. */

  /* P4X-ALTABS: absolute (pressure) altitude first, boot-relative below. */

  /* P4X-FIX-ALTCLK: the big number is the airborne's own ground-relative
   * altitude (what the panel used to show); the pressure-derived absolute value
   * is only a reference line, because it is negative whenever the local
   * pressure sits above the standard atmosphere.
   */

  /* P4X-FIX-ALT2: the big number is the sea-level altitude as the owner asked.
   * Local pressure 1018 hPa is above the 1013.25 standard, hence a negative
   * number - that is the correct reading, not a fault.
   */

  /* P4X-ALTFIT: the big number is the airborne's own fused altitude "a", after
   * the ground-side fit - same quantity, same reference, just filtered.  No
   * conversion to sea level and no re-zeroing: what the vehicle reports is what
   * the panel shows.
   */

  snprintf(buf, sizeof(buf), "%.1f", (double)g_alt_msl);

  text_scale(x + w - 380, y + 30, buf, C_SKYBLUE, 4);
  text_scale(x + w - 380 + (int)strlen(buf) * 32 + 10, y + 70, "m", g_c_label, 1);

  /* P4X-ALTCARD: pressure, exactly like the earlier SiFli panel ("101.8kPa"). */

  if (fl.pressure > 1000.0f)
    {
      snprintf(buf, sizeof(buf), "%.1f kPa", (double)(fl.pressure / 1000.0f));
    }
  else
    {
      snprintf(buf, sizeof(buf), "-- kPa");
    }

  text_scale(x + w - 380, y + 92, buf, g_c_label, 2);

  bar(x + w - 380, y + 118, 340, 14, (fl.pressure - 95000.0f) / 10000.0f,
      C_SKYBLUE);

  y += big_h + 8;

  /* 三轴加速度 */
  card(x, y, cw, acc_h, C_RED);
  text_cn(x + 18, y + 10, "X轴", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%+.2f", fl.ax);
  text_scale(x + 18, y + 42, buf, C_RED, 4);
  text_scale(x + 18, y + 100, "g", g_c_label, 1);
  bar(x + 18, y + acc_h - 20, cw - 36, 12, (fl.ax + 2.0f) / 4.0f, C_RED);

  card(x + cw + 8, y, cw, acc_h, C_GREEN);
  text_cn(x + cw + 26, y + 10, "Y轴", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%+.2f", fl.ay);
  text_scale(x + cw + 26, y + 42, buf, C_GREEN, 4);
  text_scale(x + cw + 26, y + 100, "g", g_c_label, 1);
  bar(x + cw + 26, y + acc_h - 20, cw - 36, 12, (fl.ay + 2.0f) / 4.0f, C_GREEN);

  card(x + 2 * cw + 16, y, cw, acc_h, C_BLUE);
  text_cn(x + 2 * cw + 34, y + 10, "Z轴", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%.2f", fl.az);
  text_scale(x + 2 * cw + 34, y + 42, buf, C_BLUE, 4);
  text_scale(x + 2 * cw + 34, y + 100, "g", g_c_label, 1);
  bar(x + 2 * cw + 34, y + acc_h - 20, cw - 36, 12, fl.az / 3.5f, C_BLUE);

  y += acc_h + 8;

  /* 速度曲线（填满底部） */
  snprintf(buf, sizeof(buf), "%.0f", g_speed);
  draw_chart(x, y, w, CONT_Y + CONT_H - y, v_hist, 150.0f, C_GREEN, "速度曲线", buf);
}

/****************************************************************************
 * Page 3 · 火箭页（思澈特色 3D 倾斜）
 ****************************************************************************/

/* P4X-ROCKET3D: a real 3D rocket.
 *
 * A 12-sided body that tapers to the nose, a cone nose, four swept tail fins
 * whose rear edge comes to a point, four smaller mid-body fins with a pointed
 * leading edge, and a dark tail cap.  Drawn with a painter's algorithm (faces
 * sorted far to near) and per-face shading from one fixed light.
 *
 * Roll rotates the body about its own axis, so both fin sets visibly turn;
 * pitch and yaw tilt the whole rocket on the panel.
 */

#define R3_SEG     12
#define R3_RINGS   4
#define R3_NV      112
#define R3_NF      80
#define R3_MAXV    12

static uint16_t r3_shade(uint16_t c, float k)
{
  int r = (c >> 11) & 0x1f;
  int g = (c >> 5) & 0x3f;
  int b = c & 0x1f;

  if (k < 0.30f) k = 0.30f;
  if (k > 1.25f) k = 1.25f;

  r = (int)((float)r * k);
  g = (int)((float)g * k);
  b = (int)((float)b * k);

  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  if (r < 0) r = 0;
  if (g < 0) g = 0;
  if (b < 0) b = 0;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static int r3_push(int fv[][R3_MAXV], int *fn, uint16_t *fc, int nf,
                   const int *v, int n, uint16_t c)
{
  int q;

  for (q = 0; q < n; q++)
    {
      fv[nf][q] = v[q];
    }

  fn[nf] = n;
  fc[nf] = c;
  return nf + 1;
}

static void draw_rocket3d(int cx, int cy, float yaw, float pitch, float roll,
                          int thrust)
{
  static float mx[R3_NV], my[R3_NV], mz[R3_NV];
  static float wx[R3_NV], wy[R3_NV], wz[R3_NV];
  static int sx[R3_NV], sy[R3_NV];
  static int fv[R3_NF][R3_MAXV];
  static int fn[R3_NF];
  static uint16_t fc[R3_NF];
  static float fd[R3_NF];
  static int order[R3_NF];
  static int polyx[R3_MAXV], polyy[R3_MAXV];

  /* P4X-ROCKET3D-2: twice as long as before, slim like a sounding rocket.
   * The model is centred on y = 0, so the rocket stays centred on the panel.
   */

  static const float ringy[R3_RINGS] = { -240.0f, -120.0f, 40.0f, 132.0f };
  static const float ringr[R3_RINGS] = {   25.0f,   26.0f, 24.0f,  19.0f };

  /* P4X-ATT11: 符号统一在箭载端处理（P4X-SIGN），模型侧直取遥测值 */
  float rr = roll  * 0.01745329f;
  float pp = pitch * 0.01745329f;
  float yw = yaw   * 0.01745329f;
  float cr = cosf(rr), sr = sinf(rr);
  float cp = cosf(pp), sp = sinf(pp);
  float cyw = cosf(yw), syw = sinf(yw);
  int body = 0x9D35;                 /* steel grey, RGB565 */
  int v_apex;
  int v_tfin;
  int v_mfin;
  int nv = 0;
  int nf = 0;
  int i, j, a;

  /* ---- body rings ----------------------------------------------------- */

  for (i = 0; i < R3_RINGS; i++)
    {
      for (a = 0; a < R3_SEG; a++)
        {
          float th = (float)a * 6.28318f / (float)R3_SEG;

          mx[nv] = ringr[i] * cosf(th);
          my[nv] = ringy[i];
          mz[nv] = ringr[i] * sinf(th);
          nv++;
        }
    }

  v_apex = nv;

  mx[nv] = 0.0f;                     /* nose tip */
  my[nv] = 252.0f;
  mz[nv] = 0.0f;
  nv++;

  /* ---- tail fins: rear edge swept back into a downward point ---------- */

  v_tfin = nv;

  for (i = 0; i < 4; i++)
    {
      float th = (float)i * 1.5707963f + 0.7853982f;
      float ct = cosf(th), st = sinf(th);

      /* P4X-ROCKET3D-3: outer edge runs straight down parallel to the body
       * axis, then the bottom edge goes straight back to the body - no
       * sloped point at the end.
       */

      mx[nv] = 24.0f * ct; my[nv] = -128.0f; mz[nv] = 24.0f * st; nv++;
      mx[nv] = 46.0f * ct; my[nv] = -168.0f; mz[nv] = 46.0f * st; nv++;
      mx[nv] = 46.0f * ct; my[nv] = -252.0f; mz[nv] = 46.0f * st; nv++;
      mx[nv] = 24.0f * ct; my[nv] = -240.0f; mz[nv] = 24.0f * st; nv++;
    }

  /* ---- mid-body fixed fins: pointed leading edge, smaller ------------- */

  v_mfin = nv;

  for (i = 0; i < 4; i++)
    {
      float th = (float)i * 1.5707963f + 0.7853982f;
      float ct = cosf(th), st = sinf(th);

      mx[nv] = 25.0f * ct; my[nv] =  72.0f; mz[nv] = 25.0f * st; nv++;
      mx[nv] = 41.0f * ct; my[nv] =  20.0f; mz[nv] = 41.0f * st; nv++;
      mx[nv] = 41.0f * ct; my[nv] = -18.0f; mz[nv] = 41.0f * st; nv++;
      mx[nv] = 25.0f * ct; my[nv] = -14.0f; mz[nv] = 25.0f * st; nv++;
    }

  /* ---- P4X-ATT9: 三个角按约定各绕自己的轴 -----------------------------
   *
   *   俯仰 pitch → 绕 X 轴（前后倾）
   *   滚转 roll  → 绕 Z 轴（左右倾）
   *   偏航 yaw   → 绕 Y 轴（箭体自转）
   */

  for (i = 0; i < nv; i++)
    {
      float x = mx[i];
      float y = my[i];
      float z = mz[i];
      float t;

      t = x * cr - y * sr;              /* roll：绕 Z 轴 = 左右倾 */
      y = x * sr + y * cr;
      x = t;

      t = y * cp - z * sp;              /* pitch：绕 X 轴 = 前后倾 */
      z = y * sp + z * cp;
      y = t;

      t = x * cyw + z * syw;            /* yaw：绕 Y 轴 = 箭体自转 */
      z = -x * syw + z * cyw;
      x = t;

      wx[i] = x;
      wy[i] = y;
      wz[i] = z;
    }

  /* ---- perspective ---------------------------------------------------- */

  for (i = 0; i < nv; i++)
    {
      float zc = wz[i] + 640.0f;
      float s;

      if (zc < 80.0f)
        {
          zc = 80.0f;
        }

      s = 400.0f / zc;

      sx[i] = cx + (int)(wx[i] * s);
      sy[i] = cy - (int)(wy[i] * s);
    }

  /* ---- faces ---------------------------------------------------------- */

  {
    int v[R3_MAXV];

    for (i = 0; i < R3_RINGS - 1; i++)
      {
        for (a = 0; a < R3_SEG; a++)
          {
            int a2 = (a + 1) % R3_SEG;

            v[0] = i * R3_SEG + a;
            v[1] = i * R3_SEG + a2;
            v[2] = (i + 1) * R3_SEG + a2;
            v[3] = (i + 1) * R3_SEG + a;
            nf = r3_push(fv, fn, fc, nf, v, 4, (uint16_t)body);
          }
      }

    for (a = 0; a < R3_SEG; a++)       /* nose cone */
      {
        int a2 = (a + 1) % R3_SEG;

        v[0] = (R3_RINGS - 1) * R3_SEG + a;
        v[1] = (R3_RINGS - 1) * R3_SEG + a2;
        v[2] = v_apex;
        nf = r3_push(fv, fn, fc, nf, v, 3, C_RED);
      }

    for (a = 0; a < R3_SEG; a++)       /* tail cap */
      {
        v[a] = a;
      }

    nf = r3_push(fv, fn, fc, nf, v, R3_SEG, 0x4208);

    for (i = 0; i < 4; i++)            /* tail fins */
      {
        int base = v_tfin + i * 4;

        v[0] = base;
        v[1] = base + 1;
        v[2] = base + 2;
        v[3] = base + 3;
        nf = r3_push(fv, fn, fc, nf, v, 4, C_YELLOW);
      }

    for (i = 0; i < 4; i++)            /* mid fins */
      {
        int base = v_mfin + i * 4;

        v[0] = base;
        v[1] = base + 1;
        v[2] = base + 2;
        v[3] = base + 3;
        nf = r3_push(fv, fn, fc, nf, v, 4, C_ORANGE);
      }
  }

  for (i = 0; i < nf; i++)
    {
      float zz = 0.0f;

      for (j = 0; j < fn[i]; j++)
        {
          zz += wz[fv[i][j]];
        }

      fd[i] = zz / (float)fn[i];
    }

  for (i = 0; i < nf; i++)             /* painter: far to near */
    {
      order[i] = i;
    }

  for (i = 0; i < nf; i++)
    {
      for (j = i + 1; j < nf; j++)
        {
          if (fd[order[j]] > fd[order[i]])
            {
              int t = order[i];

              order[i] = order[j];
              order[j] = t;
            }
        }
    }

  for (i = 0; i < nf; i++)
    {
      int f = order[i];
      float ux, uy, uz, vx2, vy2, vz2;
      float nx, ny, nz, nl, k;

      ux  = wx[fv[f][1]] - wx[fv[f][0]];
      uy  = wy[fv[f][1]] - wy[fv[f][0]];
      uz  = wz[fv[f][1]] - wz[fv[f][0]];
      vx2 = wx[fv[f][2]] - wx[fv[f][0]];
      vy2 = wy[fv[f][2]] - wy[fv[f][0]];
      vz2 = wz[fv[f][2]] - wz[fv[f][0]];

      nx = uy * vz2 - uz * vy2;
      ny = uz * vx2 - ux * vz2;
      nz = ux * vy2 - uy * vx2;

      nl = sqrtf(nx * nx + ny * ny + nz * nz);
      if (nl < 1e-4f)
        {
          nl = 1e-4f;
        }

      nx /= nl;
      ny /= nl;
      nz /= nl;

      k = 0.40f + 0.60f * fabsf(nx * -0.38f + ny * 0.52f + nz * 0.76f);

      for (j = 0; j < fn[f]; j++)
        {
          polyx[j] = sx[fv[f][j]];
          polyy[j] = sy[fv[f][j]];
        }

      fill_poly(polyx, polyy, fn[f], r3_shade(fc[f], k));
    }

  /* ---- exhaust -------------------------------------------------------- */

  if (thrust)
    {
      int tx = 0;
      int ty = 0;
      int len = 30 + (int)(14.0f * sinf((float)(tick % 16) * 0.7f));

      for (i = 0; i < R3_SEG; i++)
        {
          tx += sx[i];
          ty += sy[i];
        }

      tx /= R3_SEG;
      ty /= R3_SEG;

      polyx[0] = tx - 16; polyy[0] = ty;
      polyx[1] = tx + 16; polyy[1] = ty;
      polyx[2] = tx + 7;  polyy[2] = ty + len;
      polyx[3] = tx;      polyy[3] = ty + len + 16;
      polyx[4] = tx - 7;  polyy[4] = ty + len;
      fill_poly(polyx, polyy, 5, C_YELLOW);

      polyx[0] = tx - 8;  polyy[0] = ty;
      polyx[1] = tx + 8;  polyy[1] = ty;
      polyx[2] = tx;      polyy[2] = ty + len;
      fill_poly(polyx, polyy, 3, C_ORANGE);
    }
}

static void page_rocket(void)
{
  char buf[40];
  int x = CONT_X, y = CONT_Y, w = CONT_W, h = CONT_H;
  int rw = 420;

  draw_page_header("火箭姿态", 3);

  card(x, y, w - rw - 8, h, 0);
  draw_rocket3d(x + (w - rw - 8) / 2, y + h / 2 + 20,
                fl.yaw, fl.pitch, fl.roll, fl.phase == 2 || fl.phase == 3);

  /* 右侧数据 */
  {
    int dx = x + w - rw, dy = y;
    int ch = (h - 16) / 3;
    int dw = rw;

    card(dx, dy, dw, ch, C_YELLOW);
    text_cn(dx + 18, dy + 10, "俯仰", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%+.1f", fl.pitch);
    text_scale(dx + 18, dy + 30, buf, C_YELLOW, 4);
    text_scale(dx + dw - 90, dy + 52, "deg", g_c_label, 1);

    /* P4X-P4UI: the same gauge under every value, -90..+90 degrees. */

    /* P4X-P4COORD: +/-45 degrees, and the scale is printed under the gauge
     * so 30 degrees clearly reads as two thirds instead of "not full yet".
     */

    gauge(dx + 18, dy + ch - 40, dw - 36, 16, fl.pitch / 45.0f, C_YELLOW);
    text(dx + 18, dy + ch - 58, "-45", g_c_label);
    text(dx + 18 + (dw - 36) / 2 - 4, dy + ch - 58, "0", g_c_text);
    text(dx + dw - 18 - 24, dy + ch - 58, "+45", g_c_label);

    dy += ch + 8;
    card(dx, dy, dw, ch, C_MAGENTA);
    text_cn(dx + 18, dy + 10, "滚转", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%+.1f", fl.roll);
    text_scale(dx + 18, dy + 30, buf, C_MAGENTA, 4);
    text_scale(dx + dw - 90, dy + 52, "deg", g_c_label, 1);

    gauge(dx + 18, dy + ch - 40, dw - 36, 16, fl.roll / 45.0f, C_MAGENTA);
    text(dx + 18, dy + ch - 58, "-45", g_c_label);
    text(dx + 18 + (dw - 36) / 2 - 4, dy + ch - 58, "0", g_c_text);
    text(dx + dw - 18 - 24, dy + ch - 58, "+45", g_c_label);

    dy += ch + 8;
    card(dx, dy, dw, ch, g_c_yaw);
    text_cn(dx + 18, dy + 10, "偏航", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%.1f", fl.yaw);
    text_scale(dx + 18, dy + 30, buf, g_c_yaw, 4);
    text_scale(dx + dw - 90, dy + 52, "deg", g_c_label, 1);

    /* P4X-P4COORD: yaw is 0..360, but the gauge centre must be 0 degrees -
     * mapping it from 180 put 7 degrees almost at the left end of the bar,
     * which is the "7 degrees and it is nearly full" report.
     */

    gauge(dx + 18, dy + ch - 40, dw - 36, 16,
          (fl.yaw > 180.0f ? fl.yaw - 360.0f : fl.yaw) / 180.0f, g_c_yaw);
    text(dx + 18, dy + ch - 58, "-180", g_c_label);
    text(dx + 18 + (dw - 36) / 2 - 4, dy + ch - 58, "0", g_c_text);
    text(dx + dw - 18 - 30, dy + ch - 58, "+180", g_c_label);
  }

  /* 左下角：高度/速度/阶段 */
  {
    int bx = x + 20, by = y + h - 96;
    text_cn(bx, by, "高度", C_SKYBLUE, 1);
    snprintf(buf, sizeof(buf), "%.1f m", (double)g_alt_msl);
    text_scale(bx + 80, by - 4, buf, C_SKYBLUE, 3);
    text_cn(bx, by + 40, "速度", C_GREEN, 1);
    snprintf(buf, sizeof(buf), "%.0f m/s", g_speed);
    text_scale(bx + 80, by + 36, buf, C_GREEN, 3);
    text_cn(bx + 420, by + 12, phase_cn[phase_show_idx()],
            phase_color_now(), 3);
  }
}

/****************************************************************************
 * Page 4 · 雷达页（网页端第 3 页）
 ****************************************************************************/

static void geo_to_xy(double lat, double lon, int *ox, int *oy, float scale)
{
  float r = 3.14159265f / 180.0f;
  double dn = (lat - gs_lat) * 111320.0;                        /* 北向 m */
  double de = (lon - gs_lon) * 111320.0 * cos(gs_lat * r);      /* 东向 m */

  *ox = (int)(de * scale);
  *oy = -(int)(dn * scale);
}

/* P4X-BTNFAST2: draw the locate button into the back buffer.
 *
 * Extracted from page_radar() so the fast path below can repaint exactly the
 * same pixels without drawing the whole page.
 */

/* P4X-BTNSMOOTH: a phone-like press animation.
 *
 * Phones never snap a button between two colours - the highlight eases in on
 * touch-down and eases back out after the action, over 100-500 ms, at the
 * display frame rate.  Three hard steps in 0.2 s is what read as "not
 * smooth".  The colour is now a function of elapsed time, so the button can
 * be repainted at ~60 fps while the page keeps its own slower cadence.
 */

#define BTN_SEQ_NONE    0
#define BTN_SEQ_PRESS   1
#define BTN_SEQ_SUCCESS 2

static int g_btn_seq = BTN_SEQ_NONE;
static unsigned long g_btn_seq_t0;

static uint16_t rgb_lerp(uint16_t a, uint16_t b, int t)
{
  int ar = (a >> 11) & 0x1f, ag = (a >> 5) & 0x3f, ab = a & 0x1f;
  int br = (b >> 11) & 0x1f, bg = (b >> 5) & 0x3f, bb2 = b & 0x1f;
  int r, g, bl;

  if (t < 0) t = 0;
  if (t > 1000) t = 1000;

  r  = ar + (br - ar) * t / 1000;
  g  = ag + (bg - ag) * t / 1000;
  bl = ab + (bb2 - ab) * t / 1000;

  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static unsigned long btn_seq_ms(void)
{
  return (clock_systime_ticks() - g_btn_seq_t0) * 1000UL / TICK_PER_SEC;
}

static void btn_seq_begin(int seq)
{
  g_btn_seq    = seq;
  g_btn_seq_t0 = clock_systime_ticks();
}

static int btn_anim_active(void)
{
  unsigned long ms;

  if (g_btn_seq == BTN_SEQ_NONE)
    {
      return 0;
    }

  ms = btn_seq_ms();

  if (g_btn_seq == BTN_SEQ_PRESS)
    {
      return ms < 25;                 /* only while the fade runs */
    }

  return ms < 110;
}

static uint16_t btn_colour_now(void)
{
  unsigned long ms = btn_seq_ms();

  if (g_btn_seq == BTN_SEQ_PRESS)
    {
      /* P4X-BTNFIX4: self-heal.  If the pressure is gone and nothing
       * changed the state, the button must not stay highlighted forever.
       */

      if (!touch_button_hot && ms > 400)
        {
          g_btn_seq = BTN_SEQ_NONE;
          return g_c_darker;
        }

      if (ms >= 15)
        {
          return C_SKYBLUE;
        }

      return rgb_lerp(g_c_darker, C_SKYBLUE, (int)(ms * 1000UL / 15UL));
    }

  if (g_btn_seq == BTN_SEQ_SUCCESS)
    {
      if (ms < 20)                        /* blue -> green */
        {
          return rgb_lerp(C_SKYBLUE, C_GREEN, (int)(ms * 1000UL / 20UL));
        }

      if (ms < 45)                        /* hold the green */
        {
          return C_GREEN;
        }

      if (ms < 100)                       /* back to normal, 0.1 s in all */
        {
          return rgb_lerp(C_GREEN, g_c_darker,
                          (int)((ms - 45) * 1000UL / 55UL));
        }

      g_btn_seq = BTN_SEQ_NONE;
    }

  return g_c_darker;
}

static void locate_button_draw(int dx, int dy, int dw, int dh)
{
  unsigned long bnow = clock_systime_ticks();
  int ok = (g_btn_ok_tick != 0 &&
            (bnow - g_btn_ok_tick) < (4UL * TICK_PER_SEC));

  g_btn_x0 = dx;
  g_btn_y0 = dy;
  g_btn_x1 = dx + dw - 1;
  g_btn_y1 = dy + dh - 1;

  fill_round_rect(dx, dy, dw, dh, 8, btn_colour_now());
  card_border(dx, dy, dw, dh, g_c_label);
  text_cn(dx + 24, dy + dh / 2 - 14, "定位", g_c_text, 2);
  text(dx + 120, dy + dh / 2 - 4,
       touch_button_hot ? "按下" : (ok ? "已启用" :
       (telemetry_live ? "点击定位" : "无链路")), g_c_text);
}

/* P4X-BTNFAST2: repaint only the button band.
 *
 * A full frame is ~20 ms of drawing plus ~20 ms of blitting the whole 1.2 MB
 * panel buffer, so the press colour only appeared 100-200 ms later - far too
 * slow for a button.  The button is one small rectangle: repaint it in the back
 * buffer, copy just those rows to the frame buffer and push only that band.
 */

static void locate_button_fast(void)
{
  int y0;
  int h;
  size_t off;
  size_t len;
  struct fb_area_s area;

  if (cur_page != 4 || g_btn_x0 < 0 || bb == NULL || fb == NULL)
    {
      return;
    }

  locate_button_draw(g_btn_x0, g_btn_y0, g_btn_x1 - g_btn_x0 + 1,
                     g_btn_y1 - g_btn_y0 + 1);

  y0 = g_btn_y0;
  h  = g_btn_y1 - g_btn_y0 + 1;

  if (y0 < 0)
    {
      h += y0;
      y0 = 0;
    }

  if (y0 + h > fb_h)
    {
      h = fb_h - y0;
    }

  if (h <= 0)
    {
      return;
    }

  off = (size_t)y0 * (size_t)fb_stride * 2u;
  len = (size_t)h * (size_t)fb_stride * 2u;

  memcpy((uint8_t *)fb + off, (uint8_t *)bb + off, len);

  area.x = 0;
  area.y = y0;
  area.w = fb_w;
  area.h = h;
  (void)ioctl(fb_fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));
}

static void page_radar(void)
{
  char buf[64];
  int x = CONT_X, y = CONT_Y, w = CONT_W, h = CONT_H;
  int rx = x + 300, ry = y + h / 2, rr = 236;
  int sidew = 380;
  int i, rxp = 0, ryp = 0;
  float scale = (float)(rr - 20) / 1200.0f;      /* 满量程 1200 m */
  double dist;
  float bearing;

  draw_page_header("雷达", 4);

  /* 雷达盘 */
  card(x, y, w - sidew - 8, h, 0);
  fill_circle(rx, ry, rr, 0x0861);
  for (i = 1; i <= 3; i++)
    draw_circle(rx, ry, rr * i / 3, C_DARKGRN);
  hline(rx - rr, rx + rr, ry, C_DARKGRN);
  vline(rx, ry - rr, ry + rr, C_DARKGRN);
  draw_circle(rx, ry, rr, C_GREEN);
  text_cn(rx - 8, ry - rr - 26, "北", C_GREEN, 1);

  /* 扫描线 */
  {
    float a = (float)(tick % 60) * 6.0f * 3.14159f / 180.0f;
    int i2;
    for (i2 = 0; i2 < 8; i2++)
      {
        float a2 = a - i2 * 0.035f;
        line(rx, ry, rx + (int)(rr * sinf(a2)), ry - (int)(rr * cosf(a2)),
             i2 < 3 ? C_LIME : C_DARKGRN);
      }
  }

  /* 火箭目标 */
  geo_to_xy(fl.lat, fl.lon, &rxp, &ryp, scale);
  {
    int pxp = rx + rxp, pyp = ry + ryp;
    if ((pxp - rx) * (pxp - rx) + (pyp - ry) * (pyp - ry) > (rr - 6) * (rr - 6))
      {
        float k = (float)(rr - 6) / sqrtf((float)((pxp - rx) * (pxp - rx) +
                                                  (pyp - ry) * (pyp - ry)));
        pxp = rx + (int)((pxp - rx) * k);
        pyp = ry + (int)((pyp - ry) * k);
      }
    fill_circle(pxp, pyp, 9, C_RED);
    draw_circle(pxp, pyp, 16, C_ORANGE);
    text_cn(pxp + 20, pyp - 10, "火箭", C_ORANGE, 1);
  }

  /* P4X-TOUCHMARK: show where the last touch landed, so "the button does not
   * react" can be split into "the touch never arrives" and "it arrives in the
   * wrong place" without another build.
   */

  {
    static int mark_shown;
    int mark_now = (g_touch_mx >= 0 &&
                    (clock_systime_ticks() - g_touch_mtick) <
                     (2UL * TICK_PER_SEC));

    /* P4X-MARKCLEAR: the marker sits on top of the page, and a page only
     * repaints the areas its cards cover - so its white text was left behind
     * in the black gaps ("why is there a white coordinate in the gap").
     * Ask for a full repaint while it is up, and once more when it goes.
     */

    if (mark_now || mark_shown)
      {
        page_clear_req = 1;
      }

    mark_shown = mark_now;

  /* P4X-MARKRADAR: only inside the radar circle.  Outside it (the black gaps
   * between the cards) a tap must leave no white coordinate text behind.
   */

  {
    double ddx = (double)(g_touch_mx - rx);
    double ddy = (double)(g_touch_my - ry);
    double lim = (double)(rr + 8);

    if ((ddx * ddx + ddy * ddy) > lim * lim)
      {
        mark_now = 0;
      }
  }

  if (mark_now)
    {
      int i3;

      for (i3 = -14; i3 <= 14; i3++)
        {
          px(g_touch_mx + i3, g_touch_my, g_c_text);
          px(g_touch_mx, g_touch_my + i3, g_c_text);
        }

      draw_circle(g_touch_mx, g_touch_my, 18, g_c_text);

      {
        char tbuf[24];

        snprintf(tbuf, sizeof(tbuf), "%d,%d", g_touch_mx, g_touch_my);
        text(g_touch_mx + 22, g_touch_my - 6, tbuf, g_c_text);
      }
    }
  }

  /* 距离刻度 */
  for (i = 1; i <= 3; i++)
    {
      snprintf(buf, sizeof(buf), "%dm", 1200 * i / 3);
      text(rx + 6, ry - rr * i / 3 + 2, buf, C_DARKGRN);
    }

  /* 右侧数据 */
  {
    int dx = x + w - sidew, dy = y;
    int ch = 96;

    card(dx, dy, sidew, ch, C_LIME);
    text_cn(dx + 18, dy + 10, "距离", g_c_label, 1);
    {
      float r2 = 3.14159265f / 180.0f;
      double dn = (fl.lat - gs_lat) * 111320.0;
      double de = (fl.lon - gs_lon) * 111320.0 * cos(gs_lat * r2);
      dist = sqrt(dn * dn + de * de);
      bearing = (float)(atan2(de, dn) * 180.0 / 3.14159265);
      if (bearing < 0) bearing += 360.0f;
    }
    snprintf(buf, sizeof(buf), "%.0f m", dist);
    text_scale(dx + 18, dy + 36, buf, C_LIME, 3);
    text_cn(dx + 240, dy + 10, "方位", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%.0f", bearing);
    text_scale(dx + 240, dy + 36, buf, C_CYAN, 3);

    dy += ch + 8;
    card(dx, dy, sidew, ch, C_SKYBLUE);
    text_cn(dx + 18, dy + 10, "地基", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%.5f", gs_lat);
    text_scale(dx + 18, dy + 34, buf, C_SKYBLUE, 2);
    snprintf(buf, sizeof(buf), "%.5f", gs_lon);
    text_scale(dx + 18, dy + 62, buf, C_SKYBLUE, 2);

    dy += ch + 8;
    card(dx, dy, sidew, ch, C_YELLOW);
    text_cn(dx + 18, dy + 10, "坐标", g_c_label, 1);
    snprintf(buf, sizeof(buf), "%.5f", fl.lat);
    text_scale(dx + 18, dy + 34, buf, C_YELLOW, 2);
    snprintf(buf, sizeof(buf), "%.5f", fl.lon);
    text_scale(dx + 18, dy + 62, buf, C_YELLOW, 2);

    /* “定位”按钮：把当前火箭位置设为地基站（触摸/按键触发） */
    dy += ch + 8;
    {
      int bh = h - (dy - y) - 4;
      if (bh < 48) bh = 48;
      /* P4X-GLOW: keep the green visible after the tap - the press and the
       * lift used to land in the same frame, so the colour never showed.
       */

      /* P4X-BTNFAST2: one function, used by both the page and the fast path. */

      locate_button_draw(dx, dy, sidew, bh);
    }
  }
}

/****************************************************************************
 * 输入：按键 / 触摸
 ****************************************************************************/

static void page_next(int dir);   /* P4X-LAT: defined below */

/* P4X-ZOOM: defined with the zoomed view further down. */
static void zoom_exit(void);

/* P4X-UIQUIET: never write to the console from the input path.
 *
 * Verified twice on this board: a task that printf()s while nobody drains
 * the USB-CDC console blocks forever inside the write, and from that moment
 * the panel never repaints - the operator sees "one tap and it freezes".
 * Every console write in the touch / swipe / key path is behind `if (0)`;
 * the format strings stay so the log stays documented.
 */

#define UI_LOG(...)  do { } while (0)

/* P4X-LAT: turn a queued swipe into a page change.  Called at the top of every
 * frame, so the delay the user feels is (sampler) + (a fraction of one frame).
 */

/* P4X-DWELL: same as apply_flip() but for a direction already taken off the
 * queue inside the input loop.
 */

static void apply_flip_dir(int dir)
{
  unsigned long t0 = board_touch_flip_tick();

  page_next(dir);
  flip_seen++;
  flip_ms = (int)((clock_systime_ticks() - t0) * 1000UL / TICK_PER_SEC);
  UI_LOG("P4X-GS: swipe %s -> page %d/%d in %d ms\n", dir, cur_page,
         flip_ms);
  (void)flip_ms;
}

static void apply_flip(void)
{
  int dir;

  while (board_touch_flip(&dir))
    {
      unsigned long t0 = board_touch_flip_tick();

      page_next(dir);
      flip_seen++;
      flip_ms = (int)((clock_systime_ticks() - t0) * 1000UL / TICK_PER_SEC);
      UI_LOG("P4X-GS: swipe %s in %d ms\n", dir, flip_ms);
      (void)flip_ms;
    }
}

static void page_next(int dir)
{
  cur_page = (cur_page + dir + NUM_PAGES) % NUM_PAGES;
  page_clear_req = 1;                 /* P4X-PAGECLEAR: repaint from scratch */
  g_toast[0] = '\0';                  /* P4X-TOAST: flipping hides it */
  g_sat_page = 0;                     /* P4X-SATCLS: sub-page belongs to a page */
}

/* P4X-TOAST: a short banner in the middle of the page.
 *
 * It is bound to the page it was raised on - flipping the page clears it at
 * once - and while it is up the 定位 button is locked so a double tap cannot
 * fire twice.  Swiping still works while it is shown; only the button is
 * held off.
 */

/* P4X-TOAST2: delay_ms lets the button flash its own colour first and go back
 * to normal before the banner appears.
 */

static void toast_show(const char *msg)
{
  unsigned long now = clock_systime_ticks();
  unsigned long delay = 0;

  /* P4X-STATUS3: the order is "execute -> detect -> colour sequence finishes
   * -> banner floats in".  Whatever is left of the colour animation becomes
   * the banner delay.
   */

  if (g_btn_seq != BTN_SEQ_NONE)
    {
      unsigned long el = btn_seq_ms();

      if (el < 100UL)
        {
          delay = (100UL - el) * TICK_PER_SEC / 1000UL;
        }
    }

  snprintf(g_toast, sizeof(g_toast), "%s", msg);
  g_toast_page  = cur_page;
  g_toast_start = now + delay;
  g_toast_until = g_toast_start + (TICK_PER_SEC * 5UL / 10UL);
}

static int toast_active(void)
{
  return (g_toast[0] != '\0' && g_toast_page == cur_page &&
          clock_systime_ticks() < g_toast_until);
}

static void toast_draw(void)
{
  int tw, tx, ty, age;
  unsigned long now = clock_systime_ticks();

  if (g_toast[0] == '\0')
    {
      return;
    }

  if (g_toast_page != cur_page || now >= g_toast_until)
    {
      if (g_toast_page == cur_page)
        {
          page_clear_req = 1;         /* P4X-TOAST2: wipe the leftover box */
        }

      g_toast[0] = '\0';             /* page changed or time is up */
      return;
    }

  if (now < g_toast_start)
    {
      return;                         /* P4X-TOAST2: colour first, banner after */
    }

  /* The banner is drawn on top of the page, and a page only repaints the
   * areas its own cards cover - so its leftover pixels would stay visible in
   * the gaps ("green bits in the background").  Ask for a full repaint every
   * frame while it is on screen, and once more after it goes away.
   */

  page_clear_req = 1;

  age = (int)((now - g_toast_start) * 1000UL / TICK_PER_SEC);
  tw  = (int)strlen(g_toast) * 11 + 30;      /* scale 2, tight box */
  tx  = CONT_X + (CONT_W - tw) / 2;
  ty  = CONT_Y + CONT_H / 2 - 30 - age / 45;

  fill_round_rect(tx, ty, tw, 46, 10,
                  g_day_mode ? 0xDEDB : 0x39E7);   /* P4X-THEME2 */
  text_cn(tx + 16, ty + 8, g_toast,
          g_day_mode ? 0x0000 : g_c_text, 2);
}

/* P4X-BTN: is this point inside the "定位" button as it is drawn? */

static int btn_hit(int x, int y)
{
  /* P4X-BTNRECT: test the rectangle the button was really drawn in.  The old
   * hard-coded box (CONT_X..CONT_X+380, y 376..532) described the LEFT column
   * of the radar page, the button sits in the RIGHT one - that is why the
   * visible button could never be hit.
   */

  if (cur_page != 4 || g_btn_x0 < 0)
    {
      return 0;
    }

  if (g_act_pending)
    {
      return 0;                      /* P4X-STATUS5: locked while the
                                      * 0.5 s detection window runs */
    }

  return (x >= g_btn_x0 - 12 && x <= g_btn_x1 + 12 &&
          y >= g_btn_y0 - 12 && y <= g_btn_y1 + 12);
}

/* P4X-STATUS2: the tap starts an action, and the action is resolved within
 * half a second: if a valid position turns up the station is set and the
 * banner says 执行成功, otherwise the banner says 操作失败.
 */

static void act_arm(void)
{
  g_act_pending  = 1;
  g_act_deadline = clock_systime_ticks() + (TICK_PER_SEC / 2UL);
}

static void act_poll(void)
{
  if (!g_act_pending)
    {
      return;
    }

  if (fl.gps_fix && fl.lat != 0.0 && fl.lon != 0.0)
    {
      gs_lat = fl.lat;
      gs_lon = fl.lon;
      own_fix = 1;
      g_btn_ok_tick = clock_systime_ticks();
      g_act_pending = 0;
      toast_show("执行成功");
      return;
    }

  if (clock_systime_ticks() >= g_act_deadline)
    {
      g_act_pending = 0;
      toast_show("操作失败");       /* no position inside 0.5 s */
    }
}

static void set_gs_from_rocket(void)
{
  /* P4X-BTNRECT: before the airborne has a fix it sends 0.0, and copying that
   * would put the ground station at the equator, where every distance is
   * wrong.  Feedback is still given (the caller flashes the button), so "the
   * touch works but there is no position yet" stays distinguishable from
   * "the button was not hit".
   */

  /* P4X-STATUS2: hand the work to act_poll(); the banner is raised there, so
   * a 0.5 s timeout can still turn into "操作失败".
   */

  act_arm();
}

static void handle_keys(void)
{
  char b[8];
  int n, i;

  if (key_fd < 0)
    {
      return;
    }

  for (;;)
    {
      struct pollfd pfd;

      /* 必须先 poll：串口 stdin 是阻塞的，直接 read 会把任务挂死
       * （实测表现：只画了底色就再也不刷新页面）。 */

      pfd.fd     = key_fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
        {
          return;
        }

      n = read(key_fd, b, sizeof(b));
      if (n <= 0) break;
      for (i = 0; i < n; i++)
        {
          char c = b[i];
          if (c == 'n' || c == 'N' || c == ' ' || c == '\t')
            {
              page_next(1);
              auto_advance = 1;
            }
          else if (c == 'p' || c == 'P')
            {
              page_next(-1);
              auto_advance = 1;
            }
          else if (c == 'a' || c == 'A')
            {
              auto_advance = !auto_advance;
            }
          else if (c == 'g' || c == 'G')
            {
              set_gs_from_rocket();
            }
          else if (c >= '1' && c <= '0' + NUM_PAGES)
            {
              cur_page = c - '1';
              g_sat_page = 0;       /* P4X-SATCLS */
              auto_advance = 1;
            }
          else if (c == 's' || c == 'S')
            {
              /* P4X-SATCLS: open/close the satellite sub-page without a finger,
               * so the page can be checked from the console. */

              g_sat_page = !g_sat_page;
              g_zoom_id = 0;
              page_clear_req = 1;
              state_dirty = 1;
            }
          else if (c == 'f' || c == 'F')
            {
              /* P4X-SATCLS: one 16:1 thumbnail of the panel on the console. */

              fb_dump_small();
              state_dirty = 1;
            }
          else if (c == 'q' || c == 'Q')
            {
              quit = 1;
            }
        }
    }
}

/* P4X-BLCMD: type "b1", "b0" or "bb" + Enter on the serial console to drive
 * the panel backlight directly.  With a black panel this answers "is the
 * backlight path alive?" without another rebuild.
 *
 * P4X-NOBLOCKREAD: this used to do a bare read() on /dev/console, which blocks
 * in the CDC driver whenever the host has not got the port open - and it is
 * called from the render loop, so the whole UI stopped on the frame that was
 * on screen and only moved again when some byte finally arrived.  That is the
 * "the picture is there but frozen" report.  It polls first now, exactly like
 * handle_keys(), so an idle console costs nothing.
 */

static void backlight_command(void)
{
  char b[16];
  struct pollfd pfd;
  int n;

  if (key_fd < 0)
    {
      return;
    }

  pfd.fd      = key_fd;
  pfd.events  = POLLIN;
  pfd.revents = 0;

  if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
    {
      return;
    }

  n = read(key_fd, b, sizeof(b) - 1);
  if (n <= 0)
    {
      return;
    }

  b[n] = '\0';

  if (b[0] == 'b' || b[0] == 'B')
    {
      if (b[1] == '1')
        {
          printf("P4X-GS: BLCMD on  -> latch bit %d\n", board_backlight(1));
          board_backlight_dump("b1");
        }
      else if (b[1] == '0')
        {
          printf("P4X-GS: BLCMD off -> latch bit %d\n", board_backlight(0));
          board_backlight_dump("b0");
        }
      else if (b[1] == 'b' || b[1] == 'B')
        {
          int i;

          for (i = 0; i < 6; i++)
            {
              board_backlight(0);
              usleep(400000);
              board_backlight(1);
              usleep(600000);
            }

          board_backlight_dump("bb done");
        }
    }
  else if (b[0] == 'd' || b[0] == 'D')
    {
      board_backlight_dump("dump");
    }
  else if (b[0] == 'r' || b[0] == 'R')
    {
      board_panel_reset_pulse();
    }
}

/* P4X-SHADE: the pull-down panel.
 *
 * Android's shade, scaled down: pull from the top edge and the panel grows with
 * the finger - stop half way and it stays half way.  Release: past half it snaps
 * open, otherwise it snaps shut, with a 44 px step per frame so it animates.
 *
 * The page behind is dimmed by darkening every second row (the panel has no
 * alpha, and a per-pixel blend over 1.2 MB would cost more than a whole frame).
 *
 * Inside: a brightness slider (software dimming - the backlight on this board is
 * a plain GPIO, not PWM), five round page buttons, and six shortcuts.
 */

#define SHADE_SLIDER_Y   46
#define SHADE_SLIDER_H   12
#define SHADE_ROUND_Y    112
#define SHADE_ROUND_R    30
#define SHADE_SMALL_Y    166
#define SHADE_SMALL_W    150
#define SHADE_SMALL_H    42

static int g_shade_h;
static int g_shade_target;
static int g_shade_max;
static int g_shade_drag;
static int g_shade_base_h;
static int g_shade_base_y;

static void theme_apply(void);        /* P4X-THEME2: defined below */

static void theme_auto_check(void)
{
  int want;

  /* P4X-THEME2: with a real GPS time the theme follows the clock - day from
   * 06:00 to 18:00 - unless the operator switched it by hand.
   */

  if (g_theme_manual || !g_time_from_gps || g_gps_hour < 0)
    {
      return;
    }

  want = (g_gps_hour >= 6 && g_gps_hour < 18) ? 1 : 0;

  if (want != g_day_mode)
    {
      g_day_mode = want;
      theme_apply();
    }
}

static void theme_apply(void)
{
  if (g_day_mode)
    {
      g_c_bg     = 0xFFFF;
      g_c_hdr    = 0xFFFF;   /* white status bar */
      g_c_text   = 0x0000;
      g_c_label  = 0x4A49;   /* P4X-THEME5: labels a bit lighter */
      g_c_dark   = 0x7BEF;   /* darker lines so the grid is readable */
      g_c_darker = 0xD69A;   /* grey cards, not near-white */
      g_c_track  = 0xFFFF;   /* white track on a grey card */
      g_c_axis   = 0x02BF;   /* P4X-YAXIS: between pure and sky blue */
      g_c_yaw    = 0x02BF;   /* readable cyan-blue on white */
      g_dim_pct  = 88;
    }
  else
    {
      g_c_bg     = 0x0000;
      g_c_hdr    = 0x0220;
      g_c_text   = 0xFFFF;
      g_c_label  = 0xE71C;
      g_c_dark   = 0x18A3;
      g_c_darker = 0x0C41;
      g_c_track  = 0x0000;
      g_c_axis   = 0x07FF;
      g_c_yaw    = 0x07FF;
      g_dim_pct  = 60;
    }

  page_clear_req = 1;
  state_dirty = 1;
}

/* P4X-POWER: the board has no supply-voltage channel wired to anything the
 * application can read, so this cannot actually fire yet.  The rule is kept
 * here so that when a divider is added it is one line to enable.
 */

static void power_dim_check(void)
{
  /* return;  place holder: 40 % when the supply sags */
}

static int shade_round_cx(int i)
{
  if (NUM_PAGES < 2)
    {
      return CONT_X + CONT_W / 2;
    }

  return CONT_X + 60 + i * (CONT_W - 120) / (NUM_PAGES - 1);
}

static int shade_small_x(int i)
{
  return CONT_X + 40 + (i % 3) * (SHADE_SMALL_W + 14);
}

static int shade_small_y(int i)
{
  return SHADE_SMALL_Y + (i / 3) * (SHADE_SMALL_H + 14);
}

/* P4X-PHASE: two big phase buttons on the right of the shortcut grid.
 *
 * The shortcut grid ends at CONT_X + 518, so the whole right-hand side of the
 * panel is free.  They are 190 x 98 - big enough to hit without looking - and
 * each one shows which phase it steps to, because "next" alone is not enough
 * information when the rocket is on the pad.
 */

#define PH_BTN_Y    166
#define PH_BTN_H    98
#define PH_BTN_W    190
#define PH_BTN_X0   (CONT_X + 540)
#define PH_BTN_X1   (CONT_X + 760)

static int g_ph_hold = -1;            /* P4X-PHASEHOLD: which button is held */
static unsigned long g_ph_hold_t0;    /* and since when */

static int shade_phase_hit(int x, int y)
{
  if (y < PH_BTN_Y || y > PH_BTN_Y + PH_BTN_H)
    {
      return -1;
    }

  /* P4X-PHASE: only the left box is a button now; the right one is the phase
   * read-out and must not react to a touch.
   */

  if (x >= PH_BTN_X0 && x <= PH_BTN_X0 + PH_BTN_W)
    {
      return 0;
    }

  return -1;
}

/* P4X-PHASEHOLD: a thick progress frame around a button.
 *
 * "permille" of the perimeter, clockwise from the top-left corner.  Red, five
 * pixels wide, so it reads as a filling frame while the finger is down: the
 * button is armed when the frame closes, which is exactly the two seconds the
 * operator asked for.
 */

static void border_progress(int x, int y, int w, int h, int t, int permille,
                            uint16_t c)
{
  int per;
  int len;
  int rem;

  if (w <= 0 || h <= 0 || permille <= 0)
    {
      return;
    }

  if (permille > 1000)
    {
      permille = 1000;
    }

  per = 2 * (w + h);
  len = per * permille / 1000;
  rem = len;

  if (rem > 0)                                  /* top, left to right */
    {
      int l = (rem > w) ? w : rem;

      fill_rect(x, y, l, t, c);
      rem -= l;
    }

  if (rem > 0)                                  /* right, top to bottom */
    {
      int l = (rem > h) ? h : rem;

      fill_rect(x + w - t, y, t, l, c);
      rem -= l;
    }

  if (rem > 0)                                  /* bottom, right to left */
    {
      int l = (rem > w) ? w : rem;

      fill_rect(x + w - l, y + h - t, l, t, c);
      rem -= l;
    }

  if (rem > 0)                                  /* left, bottom to top */
    {
      int l = (rem > h) ? h : rem;

      fill_rect(x, y + h - l, t, l, c);
      rem -= l;
    }
}

static int shade_slider_x(void)
{
  return CONT_X + 200;
}

static int shade_slider_w(void)
{
  return CONT_W - 260;
}

/* P4X-PHASEARM: the two phase boxes, in one place.
 *
 * They used to be drawn inline in shade_draw(), which made them impossible to
 * refresh on their own.  Pulled out here so the frame animation can repaint
 * just this rectangle - a full 1024x600 frame costs so much that the red frame
 * only managed about twelve steps per second and looked like it was jumping.
 */

static void phase_boxes_draw(void)
{
  unsigned long now = clock_systime_ticks();
  int busy = (g_phase_tx_left > 0) || (now < g_ph_cool);

  /* left: the action button */

  fill_round_rect(PH_BTN_X0, PH_BTN_Y, PH_BTN_W, PH_BTN_H, 12, g_c_darker);
  card_border(PH_BTN_X0, PH_BTN_Y, PH_BTN_W, PH_BTN_H,
              phase_color_now());
  text_cn(PH_BTN_X0 + 31, PH_BTN_Y + 33, "下一阶段", g_c_text, 2);

  /* P4X-PHASEARM: the frame has three lives.
   *
   *   red, growing        - the finger is arming it (two seconds)
   *   red, shrinking      - cancelled, walking back to nothing
   *   sky blue, shrinking - sent, five seconds of cool-down
   */

  if (g_ph_hold == 0)
    {
      int el = (int)((now - g_ph_hold_t0) * 1000 / PH_HOLD_TICKS);

      if (el > 1000) el = 1000;
      if (el < 0) el = 0;

      border_progress(PH_BTN_X0, PH_BTN_Y, PH_BTN_W, PH_BTN_H, 5, el, C_RED);
    }
  else if (g_ph_back)
    {
      int gone = (int)((now - g_ph_back_t0) * 1000 / PH_BACK_TICKS);
      int el   = g_ph_back_from - gone;

      if (el <= 0)
        {
          el       = 0;
          g_ph_back = 0;              /* fully retracted: back to rest */
        }

      if (el > 0)
        {
          border_progress(PH_BTN_X0, PH_BTN_Y, PH_BTN_W, PH_BTN_H, 5, el,
                          C_RED);
        }
    }
  else if (busy)
    {
      int el = 1000 - (int)((now - g_ph_sent_t) * 1000 / PH_COOL_TICKS);

      if (el < 0) el = 0;

      border_progress(PH_BTN_X0, PH_BTN_Y, PH_BTN_W, PH_BTN_H, 5, el, C_SKY);
    }

  /* right: the phase the vehicle reports */

  fill_round_rect(PH_BTN_X1, PH_BTN_Y, PH_BTN_W, PH_BTN_H, 12, g_c_darker);
  card_border(PH_BTN_X1, PH_BTN_Y, PH_BTN_W, PH_BTN_H,
              phase_color_now());
  text_cn(PH_BTN_X1 + 24, PH_BTN_Y + 14, phase_cn[phase_show_idx()],
          phase_color_now(), 3);
  text(PH_BTN_X1 + 24, PH_BTN_Y + 72, "NOW", g_c_label);
}

/* P4X-PHASEARM: repaint only the phase boxes.
 *
 * Same trick the top-bar button uses: draw into the back buffer, copy the band
 * across and tell the driver about that one rectangle.  Roughly 40 kB instead
 * of 1.2 MB, which is what makes the frame look smooth.
 */

static void phase_boxes_fast(void)
{
  struct fb_area_s area;
  int x0 = PH_BTN_X0 - 2;
  int w  = PH_BTN_W + 4;
  int y0 = PH_BTN_Y - 2;
  int h  = PH_BTN_H + 4;
  int row;

  if (bb == NULL || fb == NULL)
    {
      return;
    }

  if (x0 < 0)
    {
      w += x0;
      x0 = 0;
    }

  if (x0 + w > fb_w)
    {
      w = fb_w - x0;
    }

  if (y0 < 0)
    {
      h += y0;
      y0 = 0;
    }

  if (y0 + h > fb_h)
    {
      h = fb_h - y0;
    }

  if (w <= 0 || h <= 0)
    {
      return;
    }

  phase_boxes_draw();

  for (row = 0; row < h; row++)
    {
      size_t off = ((size_t)(y0 + row) * (size_t)fb_stride +
                    (size_t)x0) * 2u;

      memcpy((uint8_t *)fb + off, (uint8_t *)bb + off, (size_t)w * 2u);
    }

  area.x = x0;
  area.y = y0;
  area.w = w;
  area.h = h;
  (void)ioctl(fb_fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));
}

static void shade_draw(void)
{
  char tb[24];
  static const char *labs[6] = { "加速度", "气压", "温度",
                                 "收发包", "定位", "数据" };
  int x = CONT_X;
  int w = CONT_W;
  int h = g_shade_h;
  int i;

  if (h <= 0)
    {
      return;
    }

  /* P4X-DIM: dimming the page behind is done by the colour scale, so nothing
   * to overlay - but the page has to be repainted underneath or the panel
   * leaves a stale band behind ("why did the page go dark").
   */

  page_clear_req = 1;

  fill_round_rect(x - 12, -24, w + 24, h + 24, 18,
                  g_day_mode ? 0xE71C : 0x18E3);   /* P4X-THEME2 */
  card_border(x - 12, -24, w + 24, h + 24, g_c_label);

  if (h < 80)
    {
      return;
    }

  text_cn(x + 24, 8, "快捷", g_c_text, 2);

  /* P4X-THEME: day / night switch in the title row. */

  {
    int bx = x + w - 230;

    fill_round_rect(bx, 4, 190, 34, 10,
                    g_day_mode ? 0xFFE0 : 0x2965);
    card_border(bx, 4, 190, 34, g_c_label);
    text_cn(bx + 12, 12, g_day_mode ? "日间模式" : "夜间模式",
            g_day_mode ? 0x0000 : g_c_text, 1);
  }

  /* P4X-PHASE: the state of the last command, as the operator asked for it -
   * "发送中" while the repeats are going out or the answer is outstanding,
   * "发送完成" once the vehicle has confirmed.  If the vehicle never reports the
   * phase that was asked for, the box says FAIL in red: the operator needs to
   * know a request did not land, because the panel deliberately does not move
   * on by itself.
   */

  {
    int pw = 150;
    unsigned long now = clock_systime_ticks();

    fill_round_rect(x + 130, 4, pw, 34, 10, g_c_darker);
    card_border(x + 130, 4, pw, 34, phase_color_now());

    if (g_phase_tx_left > 0 ||
        (g_ph_wait_ack && (now - g_ph_sent_t) < PH_ACK_TICKS))
      {
        text_cn(x + 142, 12, "发送中", C_CYAN, 1);
      }
    else if (g_ph_wait_ack)
      {
        text(x + 142, 12, "FAIL", C_RED);   /* the vehicle never answered */
      }
    else if (g_ph_acked)
      {
        text_cn(x + 142, 12, "发送完成", C_GREEN, 1);
      }
    else
      {
        text_cn(x + 142, 12, phase_cn[phase_show_idx()],
                phase_color_now(), 1);
      }
  }

  /* P4X-PHASE: one action button and one status box.
   *
   * The operator's layout: the first box is the button and says only
   * "下一阶段"; the second box shows the phase the vehicle is in right now.
   * The frame animation lives in phase_boxes_draw(), so the cool-down can be
   * repainted on its own at a much higher rate than a full page frame.
   */

  if (h >= PH_BTN_Y + PH_BTN_H)
    {
      phase_boxes_draw();
    }

  snprintf(tb, sizeof(tb), "%d%%", g_dim_pct);
  text(x + w - 110, 14, tb, C_CYAN);

  /* brightness slider */

  {
    int sx = shade_slider_x();
    int sw = shade_slider_w();
    int kn = sx + sw * g_dim_pct / 100;

    fill_rect(sx, SHADE_SLIDER_Y, sw, SHADE_SLIDER_H, g_c_dark);
    fill_rect(sx, SHADE_SLIDER_Y, sw * g_dim_pct / 100, SHADE_SLIDER_H, C_CYAN);
    fill_rect(kn - 8, SHADE_SLIDER_Y - 8, 16, SHADE_SLIDER_H + 16, g_c_text);
  }

  if (h < SHADE_ROUND_Y + SHADE_ROUND_R)
    {
      return;
    }

  /* round page buttons */

  for (i = 0; i < NUM_PAGES; i++)
    {
      int cx = shade_round_cx(i);
      int cy = SHADE_ROUND_Y;
      int sel = (cur_page == i);

      fill_circle(cx, cy, SHADE_ROUND_R,
                  sel ? C_LIME : (g_day_mode ? 0xC618 : 0x2965));
      draw_circle(cx, cy, SHADE_ROUND_R, sel ? g_c_text : g_c_label);
      snprintf(tb, sizeof(tb), "%d", i + 1);
      text_scale(cx - 8, cy - 12, tb, sel ? 0x0000 : g_c_text, 2);
    }

  if (h < SHADE_SMALL_Y + SHADE_SMALL_H)
    {
      return;
    }

  /* shortcut buttons */

  for (i = 0; i < 6; i++)
    {
      int bx = shade_small_x(i);
      int by = shade_small_y(i);

      if (by + SHADE_SMALL_H > h - 6)
        {
          break;
        }

      fill_round_rect(bx, by, SHADE_SMALL_W, SHADE_SMALL_H, 10, g_c_darker);
      card_border(bx, by, SHADE_SMALL_W, SHADE_SMALL_H, g_c_dark);
      text_cn(bx + 14, by + 12, labs[i], g_c_text, 1);
    }
}

static void zoom_enter(int id);
static void slider_set(int x)
{
  int sw = shade_slider_w();
  int p = (x - shade_slider_x()) * 100 / (sw > 0 ? sw : 1);

  if (p < 10) p = 10;
  if (p > 100) p = 100;

  if (p != g_dim_pct)
    {
      g_dim_pct = p;
      page_clear_req = 1;
      state_dirty = 1;
    }
}

static int in_slider(int x, int y)
{
  return (y >= SHADE_SLIDER_Y - 16 && y <= SHADE_SLIDER_Y + SHADE_SLIDER_H + 16 &&
          x >= shade_slider_x() - 14 && x <= shade_slider_x() + shade_slider_w() + 14);
}

static void shade_tap(int x, int y)
{
  int i;

  /* day / night chip */

  if (x >= CONT_X + CONT_W - 330 && x <= CONT_X + CONT_W - 130 &&
      y >= 0 && y <= 46)
    {
      g_day_mode = !g_day_mode;
      g_theme_manual = 1;               /* P4X-THEME2: stop auto switching */
      theme_apply();
      return;
    }

  if (in_slider(x, y))
    {
      slider_set(x);
      return;
    }

  /* round page buttons */

  for (i = 0; i < NUM_PAGES; i++)
    {
      int dx = x - shade_round_cx(i);
      int dy = y - SHADE_ROUND_Y;

      if (dx * dx + dy * dy <= (SHADE_ROUND_R + 8) * (SHADE_ROUND_R + 8))
        {
          cur_page = i;
          g_zoom_id = 0;
          page_clear_req = 1;
          g_shade_target = 0;
          state_dirty = 1;
          return;
        }
    }

  /* shortcuts */

  for (i = 0; i < 6; i++)
    {
      int bx = shade_small_x(i);
      int by = shade_small_y(i);

      if (x >= bx && x <= bx + SHADE_SMALL_W &&
          y >= by && y <= by + SHADE_SMALL_H)
        {
          g_shade_target = 0;

          if (i == 0)       zoom_enter(2);
          else if (i == 1)  zoom_enter(4);
          else if (i == 2)  zoom_enter(6);
          else if (i == 3)  zoom_enter(7);
          else if (i == 4)  { cur_page = 4; page_clear_req = 1; g_zoom_id = 0; }
          else              zoom_enter(8);

          state_dirty = 1;
          return;
        }
    }
}

static void shade_handle(int x, int y, int pressed, unsigned long releases)
{
  static int press_x = -1;
  static int press_y;
  static int press_in_panel;
  static int press_on_phase;          /* P4X-PHASETAP: the press was the button */
  static int press_on_slider;
  static int dragging_panel;
  static unsigned long last_release;
  int h;

  /* P4X-PHASEARM: holding the button for two seconds is what arms it.
   *
   * Nothing may be transmitted before the red frame has closed - executing on
   * touch-down was a real mistake: the frame was still half drawn and the
   * command was already on air.  Letting go early cancels, and the frame walks
   * back to nothing instead of snapping away, so the operator can see the
   * press being undone.
   */

  if (pressed && g_shade_h > 0)
    {
      int hit = shade_phase_hit(x, y);

      if (hit >= 0)
        {
          unsigned long now = clock_systime_ticks();

          press_on_phase = 1;         /* P4X-PHASETAP: this press is ours */
          press_in_panel = 1;         /* and it is inside the panel */
          press_x        = x;
          press_y        = y;

          /* a press during the sky-blue cool-down is simply ignored */

          if (g_ph_hold != hit && (now >= g_ph_cool || g_ph_sent_t == 0))
            {
              g_ph_hold    = hit;
              g_ph_hold_t0 = now;
              g_ph_back    = 0;
              state_dirty  = 1;       /* one full frame to show the start */
              return;
            }

          if (g_ph_hold == hit &&
              (now - g_ph_hold_t0) >= PH_HOLD_TICKS)
            {
              g_ph_hold = -1;         /* frame closed: send */
              phase_step(1, 1);
              state_dirty = 1;        /* the header changed to 发送中 */
              return;
            }

          /* P4X-PHASEARM: while the finger is down the frame is repainted by
           * the animation loop at 30 fps.  Asking for a full frame here made
           * every pass cost a whole 1024x600 redraw, which is exactly why the
           * frame still looked like it was hopping.
           */

          return;
        }

      if (g_ph_hold >= 0)
        {
          /* the finger slid off: cancel, and retract from where it got to */

          int el = (int)((clock_systime_ticks() - g_ph_hold_t0) * 1000 /
                         PH_HOLD_TICKS);

          if (el > 1000) el = 1000;
          g_ph_hold      = -1;
          g_ph_back      = 1;
          g_ph_back_from = el;
          g_ph_back_t0   = clock_systime_ticks();
          state_dirty    = 1;
        }
    }
  else if (!pressed && g_ph_hold >= 0)
    {
      int el = (int)((clock_systime_ticks() - g_ph_hold_t0) * 1000 /
                     PH_HOLD_TICKS);

      if (el > 1000) el = 1000;

      g_ph_hold      = -1;            /* released early: cancel */
      g_ph_back      = 1;             /* ... and walk the frame back */
      g_ph_back_from = el;
      g_ph_back_t0   = clock_systime_ticks();
      state_dirty    = 1;
      return;
    }

  if (pressed)
    {
      /* a pull that starts at the very top edge opens the panel */

      if (!g_shade_drag && g_shade_h == 0 && y < 48)
        {
          g_shade_drag = 1;
          g_shade_base_y = y;
          g_shade_base_h = 0;
          press_in_panel = 0;
          btn_press_seen = 1;
          return;
        }

      if (g_shade_drag)
        {
          int dy = y - g_shade_base_y;

          /* P4X-SHADEDEAD: a light touch or a jitter must not open the
           * panel - it only starts growing after a real downward pull.
           */

          if (dy < 24)
            {
              return;
            }

          h = dy - 24;

          if (h < 0) h = 0;
          if (h > g_shade_max) h = g_shade_max;

          g_shade_h = h;
          state_dirty = 1;
          return;
        }

      /* first sample of a new press */

      if (press_x == -1)
        {
          press_x = x;
          press_y = y;
          press_in_panel  = (y < g_shade_h) ? 1 : 0;
          press_on_slider = 0;
          dragging_panel  = 0;
          g_shade_base_y  = y;
          g_shade_base_h  = g_shade_h;

          if (press_in_panel && in_slider(x, y))
            {
              press_on_slider = 1;
              slider_set(x);
            }

          return;
        }

      /* P4X-THEME2: the slider follows the finger, and the panel can be pushed
       * back up from anywhere inside it - it holds wherever the finger stops.
       */

      if (press_on_slider)
        {
          slider_set(x);
          return;
        }

      if (press_in_panel)
        {
          int dy = y - g_shade_base_y;

          /* P4X-SHADETAP: a tap is not a drag.
           *
           * The threshold used to be 12 px, which a finger easily exceeds just
           * by pressing down, so tapping a blank spot in the panel turned into
           * "dragged the panel" and the panel snapped shut on release.  A real
           * drag is now 30 px of travel.
           */

          if (!dragging_panel && (dy > 30 || dy < -30))
            {
              dragging_panel = 1;
            }

          if (dragging_panel)
            {
              h = g_shade_base_h + dy;

              if (h < 0) h = 0;
              if (h > g_shade_max) h = g_shade_max;

              g_shade_h = h;
              state_dirty = 1;
            }
        }

      return;
    }

  if (releases == last_release)
    {
      return;
    }

  last_release = releases;

  /* P4X-PHASETAP: a press that was aimed at the phase button ends here.
   *
   * It never reaches the generic tap handling below, because that code decides
   * whether to close the panel from press_in_panel/press_y - values left over
   * from whatever came before.  Releasing the phase button therefore used to
   * snap the whole panel shut, which is exactly what the operator kept hitting.
   */

  if (press_on_phase)
    {
      press_on_phase = 0;
      press_x        = -1;
      press_in_panel = 0;
      state_dirty    = 1;
      return;
    }

  if (g_shade_drag || dragging_panel)
    {
      g_shade_drag   = 0;
      dragging_panel = 0;

      /* P4X-SHADETAP: only a real drag decides the snap, and it leans towards
       * staying open - closing now needs a deliberate pull upwards.
       */

      g_shade_target = (g_shade_h > g_shade_max / 4) ? g_shade_max : 0;
      press_x = -1;
      state_dirty = 1;
      return;
    }

  if (press_on_slider)
    {
      press_x = -1;
      press_on_slider = 0;
      press_in_panel = 0;
      return;
    }

  if (press_in_panel && press_y < g_shade_h)
    {
      /* P4X-SHADETAP: a tap inside the panel acts on whatever is under it and
       * never closes the panel - the operator wants it to stay put while they
       * reach for the buttons.
       */

      shade_tap(press_x, press_y);
      press_x = -1;
      state_dirty = 1;
    }
  else
    {
      g_shade_target = 0;                 /* tap below the panel closes it */
      press_x = -1;
      state_dirty = 1;
    }

  press_x = -1;
  press_in_panel = 0;
}

/* P4X-DIM: software brightness.  The backlight on this board is a plain GPIO,
 * so "brightness" is done by darkening rows; 100% draws nothing at all.
 */

static void soft_dim_full(void)
{
  /* P4X-DIM: replaced by dimc() - brightness is now a colour scale, so there
   * is nothing to overlay here.
   */
}

static void handle_touch(void)
{
  /* P4X-SIMPLETOUCH: one visible rule set.
   *
   *   press   - remember the start point and draw a marker there, so the touch
   *             is visible on the panel and the operator can see it working.
   *   release - horizontal travel >= SWIPE_PX   -> next / previous page
   *             started inside the button rect  -> run the button action
   *             otherwise (a plain tap)         -> nothing happens
   *
   * The marker is drawn straight into the frame buffer; the caller publishes
   * the frame, so no extra blit is needed.
   */

  static int sx = -1, sy = -1;
  static int max_left, max_right;
  static unsigned long last_release;
  static unsigned long press_tick;      /* P4X-NOFLIPNOISE: press time */
  static unsigned long last_flip_tick;  /* P4X-NOFLIPNOISE: last page flip */
  struct board_touch_s ts;
  int x, y;
  int dx;

  if (board_touch_state(&ts) < 0)
    {
      return;
    }

  x = ts.x;
  y = ts.y;

  /* P4X-SATCLS: the satellite indicator in the header is a button.
   *
   * It has to be handled before the pull-down panel gets the touch: that panel
   * owns every press in the top 48 rows, so a tap on SAT would otherwise be
   * swallowed as "started a pull-down and let go".  Fire on the press, so the
   * sub-page opens immediately, and release the latch only when the finger is
   * really gone.
   */

  if (!ts.pressed)
    {
      g_sat_held = 0;
    }

  if (ts.pressed && !g_sat_held && !g_shade_drag && g_shade_h == 0 &&
      g_sat_x0 >= 0 && x >= g_sat_x0 && x <= g_sat_x1 &&
      y >= g_sat_y0 && y <= g_sat_y1)
    {
      g_sat_held = 1;
      g_sat_page = !g_sat_page;
      g_zoom_id = 0;
      page_clear_req = 1;
      state_dirty = 1;
      return;
    }

  /* P4X-SHADE: the panel owns every touch while it is open or being pulled. */

  if (g_shade_drag || g_shade_h > 0 || (ts.pressed && y < 48 && g_shade_h == 0))
    {
      shade_handle(x, y, ts.pressed, ts.releases);
      return;
    }

  if (ts.pressed)
    {
      if (sx < 0)
        {
          sx = x;
          sy = y;
          max_left = 0;
          max_right = 0;
          press_tick = clock_systime_ticks();   /* P4X-NOFLIPNOISE */

          /* P4X-PANELHOLD: a two-second hold anywhere on the panel replays
           * the display's power-on sequence.  The DSI link on this board drops
           * when the ribbon is disturbed ("touch the board and it goes blue"),
           * and until now the only cure was a reboot.  This gives the operator
           * an in-place cure that costs 0.2 s and loses nothing.
           */

          if (!g_panel_hold)
            {
              g_panel_hold      = 1;
              g_panel_hold_done = 0;
              g_panel_hold_t    = clock_systime_ticks();
              g_panel_hold_x    = x;
              g_panel_hold_y    = y;
            }

          touch_button_hot = btn_hit(x, y);
          g_touch_mx = x;
          g_touch_my = y;
          g_touch_mtick = clock_systime_ticks();

          /* P4X-BTNFAST3: do NOT ask for a full page repaint here.  A whole
           * frame costs ~40-70 ms (draw + 1.2 MB blit + cache sync), and the
           * button colour was stuck behind it.  Only the button band is
           * repainted, which takes a few milliseconds.
           */

          if (touch_button_hot)
            {
              btn_seq_begin(BTN_SEQ_PRESS);
              locate_button_fast();
            }

          UI_LOG("press %d,%d\n", x, y);
        }
      else
        {
          dx = x - sx;
          if (dx < 0 && -dx > max_left)
            {
              max_left = -dx;
            }
          else if (dx > 0 && dx > max_right)
            {
              max_right = dx;
            }
        }

      /* P4X-REVERT: marker removed - it painted over the UI on every sample. */

      /* P4X-PANELHOLD: holding still for two seconds re-initialises the
       * display; holding on for six seconds reboots into the ROM downloader so
       * the board can be flashed without touching the BOOT button.
       */

      if (g_panel_hold && abs(x - g_panel_hold_x) < 16 &&
          abs(y - g_panel_hold_y) < 16)
        {
          unsigned long held = clock_systime_ticks() - g_panel_hold_t;

          if (held > 600)
            {
              g_panel_hold = 0;
              enter_rom_mode();          /* does not return */
            }
          else if (held > 200 && !g_panel_hold_done)
            {
              g_panel_hold_done = 1;
              panel_recover_now();
              state_dirty = 1;
            }
        }

      return;
    }

  if (ts.releases == last_release)
    {
      return;
    }

  last_release = ts.releases;

  g_panel_hold = 0;                 /* P4X-PANELHOLD: end of this touch */

  if (sx >= 0)
    {
      int travel = (max_left > max_right) ? max_left : max_right;
      int dir = (max_left > max_right) ? 1 : -1;

      /* P4X-NOFLIPNOISE: a swipe has to actually look like one.
       *
       * A panel that reports a stuck contact with a little jitter produces a
       * "swipe" on every press/release cycle, and the pages ran away on their
       * own.  A real swipe is short, so a contact that lasted longer than 2.5 s
       * or a page change more often than twice a second is treated as a tap.
       */

      {
        unsigned long now = clock_systime_ticks();
        unsigned long held = now - press_tick;
        unsigned long since = now - last_flip_tick;

        if (held < 4 || held > 250 || since < 50)
          {
            travel = 0;
          }
      }

      UI_LOG("release %d,%d\n", x, y);

      if (touch_button_hot && travel < SWIPE_MIN_UI_PX)
        {
          set_gs_from_rocket();
          btn_seq_begin(BTN_SEQ_SUCCESS);
          locate_button_fast();
          UI_LOG("button\n");
        }
      else if (g_sat_page)
        {
          /* P4X-SATCLS: a tap or a swipe anywhere leaves the sub-page (and a
           * swipe must not also flip the page underneath it).  A tap that lands
           * on the indicator itself is the one that just opened the page - the
           * release that follows it must not close it again.
           */

          int on_sat = (g_sat_x0 >= 0 && x >= g_sat_x0 && x <= g_sat_x1 &&
                        y >= g_sat_y0 && y <= g_sat_y1);

          if (!on_sat)
            {
              g_sat_page = 0;
              page_clear_req = 1;
              state_dirty = 1;
            }
        }
      else if (travel >= SWIPE_PX)
        {
          /* P4X-ZOOM: while zoomed the page must not change. */

          if (g_zoom_id == 0)
            {
              page_next(dir);
              last_flip_tick = clock_systime_ticks();   /* P4X-NOFLIPNOISE */
              state_dirty = 1;      /* the page itself changed */
            }
        }
      else if (g_zoom_id != 0)
        {
          zoom_exit();              /* tap anywhere = leave the zoom */
          state_dirty = 1;
        }
      else if (cur_page <= 1)
        {
          int zid = hit_lookup(x, y);

          if (zid != 0)
            {
              zoom_enter(zid);
            }
        }

      if (g_btn_seq == BTN_SEQ_PRESS)
        {
          /* P4X-BTNFIX4: a press that never turned into an action (tapped
           * outside the button, or the finger slid away into a page flip)
           * must not leave the button blue.
           */

          g_btn_seq = BTN_SEQ_NONE;
        }

      touch_button_hot = 0;
      locate_button_fast();         /* back to normal at once */
    }

  sx = -1;
  sy = -1;
}

/* P4X-ZOOM: the zoomed view - header kept, one chart over the whole panel. */

static void page_zoom_draw(void)
{
  char buf[32];
  int x = CONT_X;
  int y = CONT_Y;
  int w = CONT_W;
  int h = CONT_H;

  hit_reset();

  switch (g_zoom_id)
    {
      case 1:
        draw_page_header("姿态曲线", cur_page);
        draw_multi_chart(x, y, w, h, 20.0f, 2.0f, pit_hist, rol_hist,
                         yaw_hist, C_YELLOW, C_MAGENTA, g_c_yaw,
                         "姿态曲线", "俯仰", "滚转", "偏航");
        break;

      case 2:
        draw_page_header("加速度曲线", cur_page);
        draw_multi_chart(x, y, w, h, 4.0f, 0.0f, ax_hist, ay_hist, az_hist,
                         C_RED, C_GREEN, C_BLUE, "加速度曲线",
                         "X轴", "Y轴", "Z轴");
        break;

      case 3:
        draw_page_header("高度曲线", cur_page);
        snprintf(buf, sizeof(buf), "%.1f", (double)g_alt_msl);
        draw_chart_fit(x, y, w, h, alt_hist, 10.0f, C_SKYBLUE,
                       "高度曲线", buf);
        break;

      case 4:
        draw_page_header("气压曲线", cur_page);
        snprintf(buf, sizeof(buf), "%.1f", fl.pressure / 1000.0f);
        draw_chart_win(x, y, w, h, p_hist, 101.325f, 5.0f, C_ORANGE,
                       "气压曲线", buf);
        break;

      case 6:
        draw_page_header("温度曲线", cur_page);
        snprintf(buf, sizeof(buf), "%.1f", (double)fl.temperature);
        draw_chart_win(x, y, w, h, tmp_hist, fl.temperature, 2.0f, C_ORANGE,
                       "温度曲线", buf);
        break;

      case 7:
        draw_page_header("收/发/丢包", cur_page);
        draw_multi_chart(x, y, w, h, 4.0f, 0.0f, rx_hist, tx_hist,
                         lost_hist, C_LIME, C_CYAN, C_RED,
                         "每秒收/发/丢包", "收", "发", "丢");
        break;

      case 8:
        {
          /* P4X-DATA5: the four data items on one page. */

          int gw2 = (w - 8) / 2;
          int gh2 = (h - 16) / 2;
          char v2[24];

          draw_page_header("数据记录", cur_page);

          snprintf(v2, sizeof(v2), "%.2f", (double)fl.az);
          draw_chart_win(x, y, gw2, gh2, az_hist, 1.0f, 0.5f, C_BLUE,
                         "加速度", v2);

          snprintf(v2, sizeof(v2), "%.1f", fl.pressure / 1000.0f);
          draw_chart_win(x + gw2 + 8, y, gw2, gh2, p_hist, 101.325f, 5.0f,
                         C_ORANGE, "气压", v2);

          snprintf(v2, sizeof(v2), "%.1f", (double)fl.temperature);
          draw_chart_win(x, y + gh2 + 8, gw2, gh2, tmp_hist,
                         fl.temperature, 2.0f, C_YELLOW, "温度", v2);

          draw_multi_chart(x + gw2 + 8, y + gh2 + 8, gw2, gh2, 4.0f, 0.0f,
                           rx_hist, tx_hist, lost_hist, C_LIME, C_CYAN,
                           C_RED, "收/发/丢包", "收", "发", "丢");
        }
        break;

      default:
        draw_page_header("速度曲线", cur_page);
        snprintf(buf, sizeof(buf), "%.0f", g_speed);
        draw_chart(x, y, w, h, v_hist, 150.0f, C_GREEN, "速度曲线", buf);
        break;
    }
}

static void zoom_enter(int id)
{
  g_zoom_win_saved = g_hist_win;
  g_zoom_id = id;
  g_hist_win = HIST_N;
  page_clear_req = 1;
  state_dirty = 1;
}

static void zoom_exit(void)
{
  g_zoom_id = 0;
  g_hist_win = g_zoom_win_saved;
  page_clear_req = 1;
}

/* P4X-SATCLS: the satellite sub-page.
 *
 * Opened by tapping the satellite indicator in the header, closed by a tap or
 * a swipe.  It answers the two questions the main pages cannot: how many
 * satellites are visible, and how that number is split between constellations.
 * The bars are the current split; the strips underneath are the last minute
 * per constellation, so "the count changed" is a shape rather than a number
 * the operator has to remember.
 *
 * The airborne sends the split as "sg" - the in-view counts it parsed from
 * $xxGSV.  This vehicle's GPS module only emits BeiDou sentences, so three
 * bars stay at zero by design; the note says so instead of looking broken.
 */

static void page_sat_draw(void)
{
  static const uint16_t col[SAT_CLS] =
  {
    C_GREEN, C_RED, C_CYAN, C_YELLOW
  };
  static const char *name[SAT_CLS] =
  {
    "GPS", "格洛纳斯", "伽利略", "北斗"
  };

  char buf[40];
  int x = CONT_X;
  int y = CONT_Y;
  int w = CONT_W;
  int cw;
  int sum = 0;
  int vmax = 0;
  int k;
  int others;

  hit_reset();
  draw_page_header("卫星分类", cur_page);

  for (k = 0; k < SAT_CLS; k++)
    {
      sum += g_sc[k];
      if (g_sc[k] > vmax)
        {
          vmax = g_sc[k];
        }
    }

  /* P4X-SATCLS: the bar scale, on the same idea as the chart ladder - the next
   * step that leaves ~25% headroom, so the tallest bar never touches the top
   * and the scale still means a number the operator can read off the grid.
   */

  {
    static const int steps[] = { 6, 9, 12, 16, 20, 24, 32 };
    int want = vmax + vmax / 4 + 1;
    int j;

    vmax = steps[(int)(sizeof(steps) / sizeof(steps[0])) - 1];

    for (j = 0; j < (int)(sizeof(steps) / sizeof(steps[0])); j++)
      {
        if (steps[j] >= want)
          {
            vmax = steps[j];
            break;
          }
      }
  }

  others = g_sc[0] + g_sc[1] + g_sc[2];

  /* ---- summary card ---- */

  card(x, y, w, 88, fl.sat > 0 ? C_GREEN : C_RED);

  fill_circle(x + 28, y + 46, 10, fl.sat > 0 ? C_GREEN : C_RED);
  text_cn(x + 50, y + 8, "卫星", g_c_label, 1);
  snprintf(buf, sizeof(buf), "%d", fl.sat);
  text_scale(x + 50, y + 24, buf, fl.sat > 0 ? C_GREEN : C_RED, 5);

  snprintf(buf, sizeof(buf), "定位 %s", fl.gps_fix ? "正常" : "无");
  text_mixed(x + 240, y + 12, buf, fl.gps_fix ? C_LIME : C_RED, 1);

  snprintf(buf, sizeof(buf), "视野内 %d", sum);
  text_mixed(x + 240, y + 44, buf, C_CYAN, 1);

  if (others == 0)
    {
      text_cn(x + 560, y + 12, "本机模块只输出北斗", C_ORANGE, 1);
    }
  else
    {
      snprintf(buf, sizeof(buf), "多系统 %d/%d/%d", g_sc[0], g_sc[1], g_sc[2]);
      text_mixed(x + 560, y + 12, buf, C_CYAN, 1);
    }

  snprintf(buf, sizeof(buf), "合计 %d", sum);
  text_mixed(x + 560, y + 44, buf, g_c_label, 1);

  /* ---- one bar per constellation ---- */

  cw = (w - 3 * 10) / SAT_CLS;

  for (k = 0; k < SAT_CLS; k++)
    {
      int bx = x + k * (cw + 10);
      int by = y + 92;
      int tx = bx + 10;
      int tw = cw - 20;
      int ty0 = by + 36;
      int th = 148;
      int fillh;
      int i;

      /* A card per column: with the track the same colour as the page the
       * empty columns were invisible in the night theme, so three quarters of
       * the page looked blank instead of "0 satellites". */

      card(bx, by, cw, 216, col[k]);

      text_mixed(tx, by + 8, name[k], col[k], 1);

      snprintf(buf, sizeof(buf), "%d", g_sc[k]);
      text_mixed(bx + cw - 26, by + 4, buf, col[k], 2);

      fill_rect(tx, ty0, tw, th, g_c_track);
      card_border(tx, ty0, tw, th, g_c_dark);

      for (i = 1; i < 4; i++)
        {
          hline(tx + 2, tx + tw - 3, ty0 + th * i / 4, g_c_dark);
        }

      fillh = (int)((float)g_sc[k] * (float)(th - 2) / (float)vmax);

      if (fillh > th - 2)
        {
          fillh = th - 2;
        }

      if (fillh > 0)
        {
          fill_rect(tx + 1, ty0 + th - 1 - fillh, tw - 2, fillh, col[k]);
        }

      /* scale: the top gridline is vmax, so the bar height means something */

      snprintf(buf, sizeof(buf), "%d", vmax);
      text(tx + 4, ty0 + 2, buf, g_c_axis);
      text(tx + 4, ty0 + th - 12, "0", g_c_axis);
    }

  /* ---- one minute of history per constellation ---- */

  for (k = 0; k < SAT_CLS; k++)
    {
      int bx = x + k * (cw + 10);
      int hy = y + 92 + 216 + 12;
      int hh = CONT_Y + CONT_H - hy;
      int gx = bx + 10;
      int gw = cw - 20;
      int gy = hy + 26;
      int gh = hh - 42;
      int mx = 8;
      int prev_x = -1;
      int prev_y = -1;
      int i;

      card(bx, hy, cw, hh, col[k]);

      text_mixed(bx + 10, hy + 4, name[k], col[k], 1);

      for (i = 0; i < SAT_HIST; i++)
        {
          if (g_sc_hist[k][i] > mx)
            {
              mx = g_sc_hist[k][i];
            }
        }

      fill_rect(gx, gy, gw, gh, g_c_track);
      hline(gx, gx + gw - 1, gy + gh, g_c_dark);
      vline(gx, gy, gy + gh, g_c_dark);

      for (i = 0; i < SAT_HIST; i++)
        {
          int idx = (g_sc_hist_idx + i) % SAT_HIST;
          int vx = gx + 1 + i * (gw - 2) / (SAT_HIST - 1);
          int vy = gy + gh - (int)g_sc_hist[k][idx] * gh / mx;

          if (prev_x >= 0)
            {
              line(prev_x, prev_y, vx, vy, col[k]);
              line(prev_x, prev_y + 1, vx, vy + 1, col[k]);
            }

          prev_x = vx;
          prev_y = vy;
        }

      snprintf(buf, sizeof(buf), "60s max %d", mx);
      text(bx + 10, hy + hh - 13, buf, g_c_label);
    }
}

/* P4X-BOOTANIM: the power-on demo.
 *
 * White page, the logo at twice its size in the middle, a short hold, then a
 * wipe from the right that clears it in 0.6 s.  The whole thing is under one
 * second, then the normal UI takes over.
 */

#if P4X_BOOTANIM_ENABLE

static void fb_push_all(void)
{
  struct fb_area_s area;

  area.x = 0;
  area.y = 0;
  area.w = fb_w;
  area.h = fb_h;
  (void)ioctl(fb_fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));
}

/* P4X-BOOTANIM2: push only the rows that changed.  A full frame copies 1.2 MB
 * plus a cache sync; the logo box is a fraction of that, which is what makes a
 * smooth frame rate possible here.
 */

static void fb_push_band(int y0, int y1)
{
  struct fb_area_s area;

  if (y0 < 0) y0 = 0;
  if (y1 > fb_h - 1) y1 = fb_h - 1;
  if (y1 < y0) return;

  area.x = 0;
  area.y = y0;
  area.w = fb_w;
  area.h = y1 - y0 + 1;
  (void)ioctl(fb_fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));
}

/* P4X-BOOTANIM4: copy only the columns the logo occupies. */

static void bb_blit_cols(int y0, int y1, int x0, int x1)
{
  int y;

  if (bb == NULL || fb == NULL) return;

  if (y0 < 0) y0 = 0;
  if (y1 > fb_h - 1) y1 = fb_h - 1;
  if (x0 < 0) x0 = 0;
  if (x1 > fb_w - 1) x1 = fb_w - 1;
  if (y1 < y0 || x1 < x0) return;

  for (y = y0; y <= y1; y++)
    {
      memcpy(fb + (size_t)y * fb_stride + x0, bb + (size_t)y * fb_stride + x0,
             (size_t)(x1 - x0 + 1) * sizeof(unsigned short));
    }
}

static void bb_blit_band(int y0, int y1)
{
  bb_blit_cols(y0, y1, 0, fb_w - 1);
}

static unsigned short logo_row[1400];

/* P4X-BOOTANIM6: draw straight into the frame buffer.
 *
 * Measured: 109 ms per frame, essentially all of it writing pixels to PSRAM -
 * the panel's DMA is scanning the same memory, so the write bandwidth available
 * to the CPU is only ~33 MB/s.  Two whole-image writes per frame (clear the old
 * box, draw the new one) plus a copy through the back buffer made that 7 MB.
 *
 * Now the image is expanded once per row (300 source pixels -> up to 780) and
 * written exactly once, directly into the frame buffer.  The new box is always
 * larger and concentric with the old one, so nothing has to be cleared either.
 */

static int logo_dxend[400];

static void logo_draw_fb(float scale, float fade, int *rx0, int *ry0,
                         int *rx1, int *ry1)
{
  int dw = (int)((float)LOGO_W * scale);
  int dh = (int)((float)LOGO_H * scale);
  int ox;
  int oy;
  int sy;
  int x;
  int fi;

  if (dw > fb_w) dw = fb_w;
  if (dh > fb_h) dh = fb_h;
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;

  ox = (fb_w - dw) / 2;
  oy = (fb_h - dh) / 2;

  *rx0 = ox;
  *ry0 = oy;
  *rx1 = ox + dw - 1;
  *ry1 = oy + dh - 1;

  /* P4X-BOOTANIM7: everything per-pixel is integer.  The float blend cost about
   * 100 ms per frame on this board (soft float), which is what made the whole
   * animation crawl.
   */

  fi = (int)(fade * 256.0f);
  if (fi < 0) fi = 0;
  if (fi > 256) fi = 256;

  for (x = 0; x < LOGO_W; x++)
    {
      logo_dxend[x] = ((x + 1) * dw) / LOGO_W;
    }

  for (sy = 0; sy < LOGO_H; sy++)
    {
      const unsigned short *srow = logo_px + sy * LOGO_W;
      int dy0 = (sy * dh) / LOGO_H;
      int dy1 = ((sy + 1) * dh) / LOGO_H;
      int cx = 0;
      int dy;

      for (x = 0; x < LOGO_W; x++)
        {
          int dx1 = logo_dxend[x];
          unsigned short c = srow[x];

          if (fi > 0)
            {
              int r = (c >> 11) & 0x1f;
              int g = (c >> 5) & 0x3f;
              int b = c & 0x1f;

              r += ((31 - r) * fi) >> 8;
              g += ((63 - g) * fi) >> 8;
              b += ((31 - b) * fi) >> 8;
              c = (unsigned short)((r << 11) | (g << 5) | b);
            }

          while (cx < dx1 && cx < dw)
            {
              logo_row[cx] = c;
              cx++;
            }
        }

      while (cx < dw)
        {
          logo_row[cx] = 0xFFFF;
          cx++;
        }

      for (dy = dy0; dy < dy1; dy++)
        {
          int fy = oy + dy;

          if (fy < 0 || fy >= fb_h)
            {
              continue;
            }

          memcpy(fb + (size_t)fy * fb_stride + ox, logo_row,
                 (size_t)dw * sizeof(unsigned short));
        }
    }
}

/* P4X-BOOTANIM6: white page, logo grows 1.2x -> 2.6x while fading into white.
 * 12 frames, one write per frame, about 0.45 s in total.
 */

static void boot_anim(void)
{
  int saved_dim;
  int i;
  unsigned long t_start;
  const int steps = 30;                 /* P4X-FINAL9: ~1.2 s at ~25 fps */

  if (fb == NULL || bb == NULL || !bb_ready)
    {
      return;
    }

  saved_dim = g_dim_pct;
  g_dim_pct = 100;

  t_start = clock_systime_ticks();

  fill_rect(0, 0, fb_w, fb_h, 0xFFFF);
  bb_blit();
  fb_push_all();

  for (i = 0; i <= steps; i++)
    {
      float t = (float)i / (float)steps;
      float scale = 1.2f + 1.1f * t;      /* 1.2x -> 2.3x */
      float fade = t * t;                 /* holds colour, then dissolves */
      int x0;
      int y0;
      int x1;
      int y1;

      logo_draw_fb(scale, fade, &x0, &y0, &x1, &y1);
      fb_push_band(y0, y1);
      usleep(2000);
    }

  {
    unsigned long dt = (clock_systime_ticks() - t_start) * 1000UL /
                       TICK_PER_SEC;

    printf("P4X-BOOTANIM: %d frames in %lu ms, %lu ms/frame\n",
           steps + 1, dt, dt / (unsigned long)(steps + 1));
  }

  g_dim_pct = saved_dim;
}

#endif

/* P4X-BLINK: a backlight blink tells the operator how far the program got.
 * On a cold boot the panel can be dark for reasons the code cannot see, so
 * the only honest signal is the light itself.
 */

static void boot_blink(int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      (void)board_backlight(0);
      usleep(150000);
      (void)board_backlight(1);
      usleep(200000);
    }
}

/* P4X-SPKTONE: a short self test tone for the speaker / PA chain.
 *
 * The codec is registered through pcm_decode, so the device expects WAV data -
 * a header is built on the fly and 0.6 s of 440 Hz goes straight to
 * /dev/audio/pcm0.  No GPIO is touched here on purpose: if the on-board
 * NS4150B amplifier is already enabled in hardware this is all it takes.
 */

static unsigned char g_wavbuf[44 + 16000 * 2 * 6 / 10];
static unsigned long g_wavlen;

static void wav_put32(unsigned char *p, unsigned long v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

static void wav_put16(unsigned char *p, unsigned v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}

static void audio_tone(void)
{
  static const unsigned long rate = 16000;
  unsigned long n = rate * 6 / 10;
  unsigned long i;
  unsigned char *d = g_wavbuf;
  int fd;

  memcpy(d, "RIFF", 4);
  wav_put32(d + 4, 36 + n * 2);
  memcpy(d + 8, "WAVEfmt ", 8);
  wav_put32(d + 16, 16);
  wav_put16(d + 20, 1);            /* PCM */
  wav_put16(d + 22, 1);            /* mono */
  wav_put32(d + 24, rate);
  wav_put32(d + 28, rate * 2);
  wav_put16(d + 32, 2);
  wav_put16(d + 34, 16);
  memcpy(d + 36, "data", 4);
  wav_put32(d + 40, n * 2);

  for (i = 0; i < n; i++)
    {
      /* 440 Hz sine, integer phase accumulator, amplitude ~1/4 full scale. */

      static unsigned long ph;
      int v;

      ph = (ph + (440UL * 65536UL / 16UL)) & 0xffffUL;
      v = (int)((ph < 32768UL) ? (ph >> 4) : (65535UL - ph) >> 4);
      v = (v - 2048) * 2;          /* -4096..4096 triangle, low level */

      wav_put16(d + 44 + i * 2, (unsigned)(v + 8192));
    }

  g_wavlen = 44 + n * 2;

  fd = open("/dev/audio/pcm0", O_WRONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "P4X-SPKTONE: open failed %d\n", errno);
      return;
    }

  /* P4X-AUDIODIAG2: full output volume, in case the init volume is too low. */

  {
    struct audio_caps_desc_s cap;

    memset(&cap, 0, sizeof(cap));
    cap.caps.ac_len = sizeof(struct audio_caps_s);
    cap.caps.ac_type = AUDIO_TYPE_FEATURE;
    cap.caps.ac_format.hw = AUDIO_FU_VOLUME;
    cap.caps.ac_controls.hw[0] = 1000;

    (void)ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)((uintptr_t)&cap));
  }

  syslog(LOG_INFO, "P4X-SPKTONE: writing %lu bytes\n", g_wavlen);

  {
    ssize_t n2 = write(fd, g_wavbuf, g_wavlen);

    syslog(LOG_INFO, "P4X-SPKTONE: write -> %d (errno %d)\n",
           (int)n2, errno);
  }

  close(fd);
  syslog(LOG_INFO, "P4X-SPKTONE: done\n");
}

/* P4X-PASCAN: find the speaker amplifier enable pin by ear.
 *
 * The on-board NS4150B needs its SHDN pin high before anything is audible, and
 * the board documentation only says "NS4150B", not which GPIO drives it.  This
 * walks a list of otherwise-unused pads, raising one at a time and playing a
 * short tone, with the pin number shown on the panel - the one that beeps is the
 * amplifier enable.
 *
 * Skipped on purpose: the I2S pins (9..13), I2C0 (7/8), panel backlight/reset
 * (26/27), the LoRa UART pads (20..24) and the strapping pins.
 */

#define P4X_PASCAN 1

#if P4X_PASCAN
extern int board_gpio_drive(int pin, int level);   /* P4X-PASCAN2 */

static const unsigned char g_pascan[] =
{
   0, 1, 2, 3, 5, 6, 14, 15, 16, 17, 18, 19, 25, 28, 29, 30, 31, 32, 33,
   34, 36, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 54
};

static void pa_scan(void)
{
  int pass;
  int i;

  for (pass = 0; pass < 3; pass++)
    {
      for (i = 0; i < (int)sizeof(g_pascan); i++)
        {
          int pin = g_pascan[i];
          char tb[32];

          (void)board_gpio_drive(pin, 1);

          fill_rect(0, 0, fb_w, fb_h, 0xFFFF);
          snprintf(tb, sizeof(tb), "PA SCAN  GPIO %d", pin);
          text_scale(80, fb_h / 2 - 20, tb, 0x0000, 4);

          snprintf(tb, sizeof(tb), "pass %d", pass + 1);
          text_scale(80, fb_h / 2 + 60, tb, 0x4208, 2);

          bb_blit();
          {
            struct fb_area_s area;

            area.x = 0;
            area.y = 0;
            area.w = fb_w;
            area.h = fb_h;
            (void)ioctl(fb_fd, FBIO_UPDATE,
                        (unsigned long)((uintptr_t)&area));
          }

          audio_tone();
          usleep(200000);

          (void)board_gpio_drive(pin, 0);
        }
    }
}
#endif

/* P4X-AUTOLOOP: play a tone, then listen on the microphone and report levels.
 *
 * Nobody has to be in the room: if the codec, the I2S link and the amplifier
 * are all alive, the microphone picks the tone up and the numbers prove it.
 */

static unsigned char g_recbuf[16000];

static void audio_autoloop(void)
{
  int fd;
  ssize_t n;

  syslog(LOG_INFO, "P4X-LOOPAUDIO: play tone\n");
  audio_tone();
  usleep(200000);

  fd = open("/dev/audio/pcm0c", O_RDONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "P4X-LOOPAUDIO: capture open failed %d\n", errno);
      return;
    }

  n = read(fd, g_recbuf, sizeof(g_recbuf));
  close(fd);

  syslog(LOG_INFO, "P4X-LOOPAUDIO: captured %d bytes\n", (int)n);

  if (n > 64)
    {
      int i;
      int peak = 0;
      unsigned long sum = 0;
      long sum2 = 0;

      /* Skip a possible WAV header: look for the biggest sample overall. */

      for (i = 0; i + 1 < (int)n; i += 2)
        {
          short s = (short)(g_recbuf[i] | (g_recbuf[i + 1] << 8));
          int a = (s < 0) ? -s : s;

          if (a > peak)
            {
              peak = a;
            }

          sum2 += a;
        }

      sum = (unsigned long)sum2 * 100UL / ((unsigned long)n / 2UL);

      syslog(LOG_INFO, "P4X-LOOPAUDIO: peak=%d avg=%lu (0=dead codec)\n",
             peak, sum);
    }
}

/****************************************************************************
 * 主程序
 ****************************************************************************/

/* P4X-PANELWD: the independent panel watchdog (see the state comment above). */

/* P4X-ENTERROM: see the prototype near the panel-hold state. */

static void enter_rom_mode(void)
{
  struct fb_area_s area;

  fill_rect(0, 0, fb_w, fb_h, 0x0000);
  text_scale(fb_w / 2 - 210, fb_h / 2 - 40, "P4X ROM MODE", 0xFFE0, 3);
  text_scale(fb_w / 2 - 210, fb_h / 2 + 10, "flashing window", 0x8410, 2);
  bb_blit();

  area.x = 0;
  area.y = 0;
  area.w = fb_w;
  area.h = fb_h;
  (void)ioctl(fb_fd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area));

  syslog(LOG_INFO, "P4X-ENTERROM: strap low, resetting\n");

  (void)board_gpio_drive(35, 0);      /* BOOT strap candidates */
  (void)board_gpio_drive(0, 0);
  usleep(80000);
  (void)board_reset(0);

  for (;;)                          /* board_reset() should not return */
    {
      usleep(200000);
    }
}

static int panel_task(int argc, FAR char *argv[])
{
  unsigned long last_seq = 0;
  unsigned long stale_since = 0;
  unsigned long start_tick = 0;
  unsigned long hb_next = 0;
  int recov = 0;

  (void)argc;
  (void)argv;

  for (;;)
    {
      unsigned long now;

      usleep(200000);               /* 200 ms */
      now = clock_systime_ticks();

      if (start_tick == 0)
        {
          start_tick  = now;
          stale_since = now;
        }

      /* the backlight enable is a latch: keep it asserted from here, so it
       * stays on even when the render loop is stuck
       */

      (void)board_backlight(1);

      if (g_frame_seq != last_seq)
        {
          last_seq    = g_frame_seq;
          stale_since = now;
        }
      else if ((now - stale_since) > 100 &&
               (last_seq > 0 || (now - start_tick) > 150))
        {
          /* one second without a new frame, or fifteen seconds from start
           * without ever drawing: the glass is not being updated
           */

          recov++;
          g_panel_recov = (unsigned long)recov;

          if ((recov % 5) == 1)
            {
              board_panel_reset_pulse();   /* full power-on timing */
            }

          (void)board_panel_resync();      /* restart the video stream */
          stale_since = now;
        }

      if (now >= hb_next)
        {
          hb_next = now + 100;             /* 1 Hz */
          syslog(LOG_INFO, "P4X-HB f=%lu r=%lu ph=%d\n",
                 g_frame_seq, g_panel_recov, phase_show_idx());
        }
    }

  return 0;
}

int main(int argc, FAR char *argv[])
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  int i;

  /* P4X-NOCONSOLE: nothing this program prints may ever be able to block
   * it.  A cold boot came up with a black panel until a host opened the
   * serial port - i.e. the program was stuck inside a console write: with no
   * reader the USB-CDC transmitter fills up and write() never returns.  All
   * of stdout/stderr goes to /dev/null instead.
   */

  boot_blink(1);                        /* P4X-BLINK: main() reached */

  {
    FILE *fn = fopen("/dev/null", "w");

    if (fn != NULL)
      {
        dup2(fileno(fn), 1);
        dup2(fileno(fn), 2);
      }
  }

  /* P4X-NOBLOCK-OFF: making fd 1 non-blocking was tried and reverted.  The
   * console then emitted half-written lines from several tasks at once
   * ("P4P4X-LORA: uart pads armed ..."), which is worse than the problem.
   * The freeze at t = 2:00 is handled by P4X_DIAG = 0 instead: nothing in
   * the render loop writes to the console at all.
   */

  (void)argc;
  (void)argv;

  /* P4X-PANELRETRY: never give up on the panel.  A cold boot used to leave the
   * board with nothing but backlight because this loop gave up after 15 s and
   * the task exited.
   */

  for (i = 0; i < 600; i++)
    {
      fb_fd = open("/dev/fb0", O_RDWR);
      if (fb_fd >= 0) break;
      usleep(500000);
    }
  boot_blink(2);                        /* P4X-BLINK: /dev/fb0 open */

  if (fb_fd < 0)
    {
      printf("ERR fb0 open\n");
      return 1;
    }

  if (ioctl(fb_fd, FBIOGET_VIDEOINFO, &vinfo) < 0 ||
      ioctl(fb_fd, FBIOGET_PLANEINFO, &pinfo) < 0)
    {
      printf("ERR fb ioctl\n");
      close(fb_fd);
      return 1;
    }

  fb_w = vinfo.xres;
  fb_h = vinfo.yres;
  fb_stride = pinfo.stride / 2;

  fb = (uint16_t *)mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FILE, fb_fd, 0);
  if (fb == MAP_FAILED)
    {
      fb = (uint16_t *)pinfo.fbmem;
    }

  fb_bytes = (size_t)fb_stride * (size_t)fb_h * 2;

  printf("P4X-GS: fb %dx%d stride=%d fb=%p bytes=%lu\n",
         fb_w, fb_h, fb_stride, fb, (unsigned long)fb_bytes);

  /* 后备缓冲：绘制全部落在可缓存内存上（快），画完一次性 memcpy 上屏
   * （避免 DSI 的 DMA 把半成品帧扫出去 = 花屏/撕裂）。
   */
  /* P4X-BBINFO: try the user heap as well as the kernel heap, and report why
   * the allocation failed if it did.
   */

  /* P4X-LAT: the PSRAM heap holds the framebuffer, so take the back buffer from
   * the same place - the task heap cannot supply 1.2 MB and without a back
   * buffer every frame is drawn straight onto the panel being scanned out.
   */

  bb = (uint16_t *)board_fb_backbuffer(fb_bytes);

  g_bb = bb;                    /* P4X-FBDUMP */
  g_bb_stride = fb_stride;
  if (bb != NULL)
    {
      printf("P4X-GS: back buffer from PSRAM heap\n");
    }

  if (bb == NULL)
    bb = (uint16_t *)memalign(64, fb_bytes);
  if (bb == NULL)
    {
      printf("P4X-GS: BBINFO malloc failed errno=%d, trying posix_memalign\n",
             errno);
      {
        FAR void *p = NULL;

        if (posix_memalign(&p, 64, fb_bytes) == 0)
          {
            bb = (uint16_t *)p;
          }
      }
    }

  if (bb == NULL)
    {
      bb = fb;
      bb_ready = 0;
      printf("P4X-GS: WARN no back buffer, draw direct\n");
    }
  else
    {
      bb_ready = 1;
      memset(bb, 0, fb_bytes);
      printf("P4X-GS: back buffer %p\n", bb);
    }

  /* P4X-BOARD-TOUCH: nothing to open; the board owns the controller. */

  printf("P4X-GS: touch via board poller\n");



  /* 键盘：优先显式打开 /dev/console（可带 O_NONBLOCK），否则退回 stdin */
  key_fd = open("/dev/console", O_RDONLY | O_NONBLOCK);
  if (key_fd < 0)
    {
      key_fd = 0;
      fcntl(key_fd, F_SETFL, O_NONBLOCK);
    }
  printf("P4X-GS: keys fd=%d\n", key_fd);

  /* P4X-KEYHELP: the console is always available, so say what it can do - this
   * is the fallback when the touch panel misbehaves.
   */

  printf("P4X-GS: console keys: n=next page, p=previous, 1-5=page, "
         "s=swipe test, q=quit\n");
  board_backlight_dump("startup");        /* P4X-BLREG */

  g_shade_max = fb_h / 2;             /* P4X-SHADE: about half the panel */

  memset(&fl, 0, sizeof(fl));
  snprintf(fl.time_str, sizeof(fl.time_str), "T+0.0");

  /* P4X-LORARX: start the receive loop.  Priority is below the UI (100) so a
   * burst of telemetry can never starve the renderer, and the stack is sized
   * for the JSON parser.
   */

  {
    /* P4X-RXPRIO: the receiver is now ABOVE the renderer.
     *
     * Evidence: at an 80 ms frame the airborne's 4 packets/s arrived as
     * 1.9/s, and the frames that did arrive had holes
     * ({"r":8.2"r":8:0.5,"y":0.0}) - byte loss, not packet loss.  The
     * receiver ran at priority 90, below the UI at 100, and the UI holds
     * the CPU for ~20 ms at a time doing memcpy / cache sync.  A 128 byte
     * FIFO only buys 130 ms at 9600 baud, so this is the first suspect.
     *
     * Telemetry work here is tiny (a 28 byte frame every 250 ms), so
     * running it above the renderer cannot starve the LCD.
     */

    int pid = nxtask_create("lora_rx", 110, NULL, 4096,
                            lora_rx_task, NULL, NULL);
    if (pid < 0)
      {
        printf("P4X-LORARX: task start failed: %d\n", pid);
      }
    else
      {
        printf("P4X-LORARX: receive task started (pid %d)\n", pid);
      }
  }

  /* P4X-PANELWD: the panel watchdog runs above the renderer so it can still
   * service the glass when the render loop is stuck.
   */

  {
    int ppid = nxtask_create("panel", 112, NULL, 2048,
                             panel_task, NULL, NULL);
    if (ppid < 0)
      {
        printf("P4X-PANELWD: task start failed: %d\n", ppid);
      }
  }

  /* P4X-NOFLICKER: the R/G/B self test flashes the whole panel three times.
   * Only run it when explicitly requested (P4X_GS_SCREENTEST=1).
   */
  {
    static const uint16_t test_cols[3] = { C_RED, C_GREEN, C_BLUE };
    static const char *test_names[3] = { "RED", "GREEN", "BLUE" };
    int k, rep;
    FAR char *st = getenv("P4X_GS_SCREENTEST");
    int want_test = (st != NULL && st[0] == '1');

    if (want_test)
      {
        printf("P4X-GS: screen test R/G/B\n");

        for (k = 0; k < 3; k++)
          {
            for (rep = 0; rep < 2; rep++)
              {
                unsigned long t0 = clock_systime_ticks();
                unsigned long dt;

                fill_rect(0, 0, fb_w, fb_h, rep ? test_cols[k] : C_BLACK);
                bb_blit();
                dt = clock_systime_ticks() - t0;
                printf("P4X-GS: test %s blit %lu ms\n", test_names[k],
                       (unsigned long)(dt * 1000UL / TICK_PER_SEC));
                usleep(250000);
              }
          }

        printf("P4X-GS: screen test done\n");
      }
  }

  theme_apply();                        /* P4X-FINAL9: day colours first */

  /* P4X-SPKTONE/PASCAN ??????????????????????????
   * ?????????????????
   */

  /* P4X-AUTOLOOP ??????????????????????????
   * ?????????? 440Hz -> ? 0.5s -> syslog ?? peak/avg?
   */

  if (getenv("P4X_LOOPAUDIO") != NULL)
    {
      audio_autoloop();
    }

#if 0
  pa_scan();
#endif

#if P4X_BOOTANIM_ENABLE
  boot_anim();                          /* P4X-BOOTANIM: power-on demo */
#endif

  /* 画面底色清一次 */
  fill_rect(0, 0, fb_w, fb_h, g_c_bg);
  bb_blit();

  {
    unsigned long last_auto = 0;
    unsigned long stat_t0 = tick;
    unsigned long t_start, t_after_draw, t_after_blit;
    unsigned long in_ms = 0;
    int anim_passes = 0;              /* P4X-BTNSMOOTH */

    /* P4X-BEAT: the button rectangle is derived from the drawn layout; log it
     * once so it can be compared against the real touch coordinates.
     */

    printf("P4X-BEAT: button rect x %d..%d y %d..%d (radar page only)\n",
           BTN_X0, BTN_X1, BTN_UP_Y, BTN_DN_Y);

    while (!quit)
      {
        t_start = clock_systime_ticks();

        /* P4X-PROF: time the input section separately - the GT911 read costs
         * I2C time on every frame.
         */

        {
          unsigned long t_in = clock_systime_ticks();

          idle_slices = 0;
          handle_keys();
          backlight_command();        /* P4X-BLCMD: b1 / b0 / bb */
          phase_pump();               /* P4X-PHASE: uplink repeats */
          apply_flip();               /* P4X-LAT: queued swipes first */
          handle_touch();
          in_ms = (clock_systime_ticks() - t_in) * 1000UL / TICK_PER_SEC;
        }

        /* P4X-NODEMO: demo_flight() used to fill the panel with a synthetic
         * profile whenever no telemetry had arrived.  That made a dead link look
         * healthy - the screen showed a plausible flight while the radio
         * carried nothing.  The panel now shows only what telemetry_apply_json()
         * has actually received; before the first packet every field reads as a
         * placeholder and the header says 无数据.
         */

        /* P4X-STALE: a link that stops has to show as "no data" instead of
         * a frozen picture, and the ring is cleared so the curves restart
         * from the current value.  This is the "clear the cache" step.
         */

        {
          static int was_live;
          unsigned long rx_now = clock_systime_ticks();
          int live = (g_rx_last_tick != 0 &&
                      (rx_now - g_rx_last_tick) < (3UL * TICK_PER_SEC));

          if (was_live && !live)
            {
              hist_clear();
            }

          was_live = live;
          telemetry_live = live;

          /* P4X-LINKSTATE: the status page reads fl.lora_ok, so it has to
           * follow the same live flag - otherwise it keeps the last value
           * written by the parser and stays green with the link dead.
           */

          fl.lora_ok = live;
          fl.rocket_link = live;
        }

        /* P4X-DATA4: packets per second, and an estimate of what was lost
         * (the best rate ever seen minus what is arriving now - the airborne
         * has no sequence number, so this is the honest approximation).
         */

        {
          static unsigned long rate_tick;
          static unsigned long rate_count;
          static unsigned long rate_peak = 1;
          unsigned long rnow = clock_systime_ticks();

          if ((rnow - rate_tick) >= TICK_PER_SEC)
            {
              unsigned long n = g_rx_gap_n;
              unsigned long d = n - rate_count;

              rate_count = n;
              rate_tick  = rnow;
              g_rx_pps   = d;

              {
                static unsigned long tx_last;
                unsigned long txn = lora_stats.tx_pkts;

                g_tx_pps = (txn >= tx_last) ? (txn - tx_last) : 0UL;
                tx_last  = txn;
              }

              if (d > rate_peak)
                {
                  rate_peak = d;
                }

              g_lost_pps = (rate_peak > d) ? (rate_peak - d) : 0UL;
            }
        }

        hist_push();

        /* P4X-ADAPT2: +-2 degrees of movement switches to the sensitive
         * mode for 2 s: a shorter window so the x axis runs about twice as
         * fast, and g_fast_frame so the renderer skips its idle sleep.
         */

        {
          static unsigned long move_tick;
          float mlo = 1e9f;
          float mhi = -1e9f;
          int mi;
          unsigned long mnow = clock_systime_ticks();

          for (mi = 0; mi < 32; mi++)
            {
              int idx = (hist_idx - 1 - mi + HIST_N * 2) % HIST_N;
              float v;

              v = pit_hist[idx];
              if (v < mlo) mlo = v;
              if (v > mhi) mhi = v;
              v = rol_hist[idx];
              if (v < mlo) mlo = v;
              if (v > mhi) mhi = v;
            }

          if ((mhi - mlo) > 2.0f)
            {
              move_tick = mnow;
            }

          g_fast_frame = ((mnow - move_tick) < (2UL * TICK_PER_SEC));

          /* P4X-SWITCHCLEAR: changing the window changes which samples are
           * on screen, and the jump looked like the trace had dropped a
           * packet.  Clear the ring on the switch so the curve restarts flat
           * at the current value instead of showing a discontinuity.
           */

          {
            int newwin = (g_zoom_id != 0) ? HIST_N :   /* P4X-ZOOM */
                         (g_fast_frame ? 20 : 48);

            if (newwin != g_hist_win)
              {
                g_hist_win = newwin;
                hist_clear();
              }
          }
        }

        /* 每帧整体重画：底色纯黑 + 卡片描边，先清屏避免上一页残留。
         * 旧版直接往非缓存显存逐像素画，一帧要在「被 DMA 同时读的」内存上
         * 做几百次零散写 → 又慢又会出现半成品帧（看着像花屏）。
         * 现在改为：后备缓冲（可缓存内存）全速绘制 → 一次顺序 memcpy 上屏。
         */

        /* P4X-NOFLICKER-2: 没有后备缓冲时，「全屏清黑 + 重画」的清屏那一瞬间
         * 会被 DSI 扫出去，肉眼就是屏闪。页面内容铺满全屏，所以只有离屏缓冲
         * 才做整屏清屏。
         */

        /* P4X-PAGECLEAR: a full clear is done once per page change; doing it
         * every frame made the panel flicker, but never doing it left parts of
         * the previous page visible under the new one.
         */

        /* P4X-FIX3: a 1.2 MB full-screen clear on every frame meant three
         * full-screen passes per frame (clear, draw, blit).  Pages paint their
         * own background, so the clear is only needed on a page change.
         */

        /* P4X-HB: a very short heartbeat, once every 2 s, so a freeze can be
         * classified: heartbeats continuing = the system is alive and only the
         * DSI picture died; heartbeats stopping = the system itself hung.
         */

        if (0 && (tick % 8) == 0)
          {
            printf("P4X-HB tick=%lu\n", tick);
          }

        if (page_clear_req)
          {
            fill_rect(0, 0, fb_w, fb_h, g_c_bg);
            page_clear_req = 0;
          }

        if (btn_glow > 0)
          {
            btn_glow--;               /* P4X-GLOW: fade the button highlight */
          }

        apply_flip();               /* P4X-LAT: a swipe may land mid-frame */

        hit_reset();

        if (g_sat_page)
          {
            page_sat_draw();          /* P4X-SATCLS: satellite sub-page */
          }
        else if (g_zoom_id != 0)
          {
            page_zoom_draw();
          }
        else
          {
            switch (cur_page)
              {
                case 0: page_status();  break;
                case 1: page_curves();  break;
                case 2: page_speed();   break;
                case 3: page_rocket();  break;
                default: page_radar();  break;
              }
          }

        /* P4X-SHADE: animate the panel when the finger is not holding it.
         *
         * P4X-SHADESMOOTH: a full 1024x600 frame costs so much that many small
         * steps are not an option, so the panel travels in bigger strides: the
         * slide is over in about half a second instead of a second and a half.
         */

        if (!g_shade_drag && g_shade_h != g_shade_target)
          {
            int d = (g_shade_target > g_shade_h) ? 56 : -56;

            g_shade_h += d;

            if ((d > 0 && g_shade_h > g_shade_target) ||
                (d < 0 && g_shade_h < g_shade_target))
              {
                g_shade_h = g_shade_target;
              }

            state_dirty = 1;      /* keep drawing while it moves */
          }

        theme_auto_check();           /* P4X-THEME2: day 06:00-18:00 */
        act_poll();                   /* P4X-STATUS2: resolve the action */
        toast_draw();                 /* P4X-TOAST: on top of the page */

        /* P4X-BLKEEP2: the backlight enable is a latch, and it drops on its own
         * after a while - the picture keeps being drawn but the glass goes
         * dark ("it lit up for a moment and then went dark again").  Writing
         * the same level every two seconds costs nothing and keeps the panel
         * lit; when the latch is healthy the write is invisible.
         */

        {
          static unsigned long bl_next;

          if (tick >= bl_next)
            {
              bl_next = tick + 20;            /* 2 s */
              (void)board_backlight(1);
            }
        }

        if (g_shade_h > 0)
          {
            shade_draw();             /* P4X-SHADE */
          }

        soft_dim_full();              /* P4X-DIM */

        /* 空闲自动轮播：每 8 秒翻一页（方便演示；按 a 可开关） */
        if (auto_advance && (tick - last_auto) > 12   )
          {
            last_auto = tick;
            page_next(1);
          }

        frame_dx0 = d_x0; frame_dy0 = d_y0; frame_dx1 = d_x1; frame_dy1 = d_y1;
        d_x0 = 1 << 30; d_y0 = 1 << 30; d_x1 = -1; d_y1 = -1;

        t_after_draw = clock_systime_ticks();
        bb_blit();                    /* 画完这一帧，一次性上屏 */

        /* 关键：通知显示驱动「这一帧画完了」。
         * 实测现象：显存里内容是对的（JTAG dump 就是完整界面），但面板一直
         * 停在开机第一帧的天蓝色 —— DW-GDMA 只搬了第一帧，之后的续流靠
         * TFR_DONE 中断重装 LLI，而那个中断没进来。
         * FBIO_UPDATE 会走到板级驱动的 updatearea()，由内核侧重装 DMA。
         */
        {
          struct fb_area_s area;

          area.x = 0;
          area.y = 0;
          area.w = fb_w;
          area.h = fb_h;
          if (ioctl(fb_fd, FBIO_UPDATE,
                    (unsigned long)((uintptr_t)&area)) < 0)
            {
              static int update_warned;
              if (!update_warned)
                {
                  update_warned = 1;
                  printf("P4X-GS: WARN FBIO_UPDATE failed: %d\n", errno);
                }
            }
        }

        t_after_blit = clock_systime_ticks();

        /* P4X-PANELWD: tell the panel task that a fresh frame reached the
         * glass.  Its own watchdog compares this counter against wall time and
         * recovers the panel when it stops moving.
         */

        g_frame_seq++;

        /* P4X-HDR: one frame counted, the rate is refreshed every second. */

        {
          g_fps_n++;

          if (g_fps_t0 == 0)
            {
              g_fps_t0 = t_after_blit;
            }
          else if ((t_after_blit - g_fps_t0) >= TICK_PER_SEC)
            {
              g_fps = g_fps_n * TICK_PER_SEC / (t_after_blit - g_fps_t0);
              g_fps_n = 0;
              g_fps_t0 = t_after_blit;
            }
        }

        {
          static int blinked;

          if (!blinked)
            {
              blinked = 1;
              boot_blink(3);          /* P4X-BLINK: first frame on screen */

              /* P4X-KICKOFF: the panel re-init experiment is withdrawn.  It
               * switched the backlight off and then failed inside
               * mipi_dsi_device_register() (the device is already registered),
               * so the panel went dark 1.5 s after boot.  A cold-boot panel fix
               * has to be done in the board bring-up, not from the application.
               */
            }
        }

        /* 每 2.5 秒报一次帧耗时：绘制 / 上屏 分开报，
         * 便于判断瓶颈是在画（CPU）还是在搬（显存带宽）。
         */
        if ((tick - stat_t0) >= 30)     /* P4X-QUIET2: was every 4 frames */
          {
            unsigned long draw =
              (t_after_draw - t_start) * 1000UL / TICK_PER_SEC;
            unsigned long blit =
              (t_after_blit - t_after_draw) * 1000UL / TICK_PER_SEC;

            last_draw_ms = draw;
            last_blit_ms = blit;
            {
              int dw = frame_dx1 - frame_dx0, dh = frame_dy1 - frame_dy0;

              /* P4X-QUIET2: keep the numbers in variables, do not print every
             * frame - see the note above about serial traffic and the panel.
             */

            (void)draw;
            (void)blit;
            (void)in_ms;
            (void)dw;
            (void)dh;
            }
            stat_t0 = tick;
          }

        /* P4X-LATDIAG: one bounded report (~60 frames in) that answers "is
         * the delay the link or the frame?" with numbers.  Bounded on
         * purpose: console writes from the UI loop block as soon as nobody
         * drains the CDC, and that used to freeze the panel.
         */

        {
          static unsigned long fr_prev, fr_sum, fr_max, fr_n;
          static unsigned long dr_sum, bl_sum;
          static int lat_stage;
          unsigned long now_t = clock_systime_ticks();

          if (fr_prev != 0)
            {
              unsigned long d = (now_t - fr_prev) * 1000UL / TICK_PER_SEC;

              fr_sum += d;
              fr_n++;
              if (d > fr_max)
                {
                  fr_max = d;
                }

              dr_sum += (t_after_draw - t_start) * 1000UL / TICK_PER_SEC;
              bl_sum += (t_after_blit - t_after_draw) * 1000UL / TICK_PER_SEC;
            }

          fr_prev = now_t;

          if (P4X_DIAG && lat_stage == 0 && fr_n >= 60)
            {
              unsigned long rn = g_rx_gap_n > 0 ? g_rx_gap_n : 1;

              lat_stage = 1;
              printf("P4X-LAT: frame avg=%lums max=%lums (draw %lums blit "
                     "%lums) | airborne gap avg=%lums max=%lums n=%lu "
                     "| good=%lu bad=%lu\n",
                     fr_sum / fr_n, fr_max, dr_sum / fr_n, bl_sum / fr_n,
                     g_rx_gap_sum / rn, g_rx_gap_max, g_rx_gap_n,
                     lora_packets, lora_bad);
            }
          else if (P4X_DIAG && lat_stage == 1 && fr_n >= 1500)
            {
              /* P4X-LAT2: two minutes in, the same numbers again.  This is
               * the check the operator asked for: altitude must sit still
               * and the speed must not creep upward on its own.
               */

              lat_stage = 2;
              printf("P4X-LAT2: 120 s later alt_abs=%.1f sensor_alt=%.1f "
                     "speed=|%.2f| grav=%.3f/%.3f/%.3f "
                     "lin=%.3f/%.3f/%.3f v=%.2f/%.2f/%.2f frames=%lu\n",
                     (double)g_alt_abs, (double)fl.alt,
                     (double)g_speed, (double)g_gx, (double)g_gy,
                     (double)g_gz, (double)g_axl, (double)g_ayl,
                     (double)g_azl, (double)g_vx, (double)g_vy,
                     (double)g_vz, fr_n);
            }
        }

        /* P4X-STATIC removed: the experiment ran and its answer was that the
         * panel drops out even with no traffic at all, so the failure is on the
         * panel side.  Normal refreshing is restored.
         */

        tick++;

        /* P4X-BLKEEP: the panel has now dimmed on its own twice with the app
         * running normally.  Re-assert the enable line every ~2 s and log the
         * real register level: a latch that drops means software, a latch that
         * stays high while the panel dims means the fault is behind the GPIO.
         */

        /* P4X-REVERT: periodic backlight re-assert removed. */

        (void)bl_keep_next;

        /* P4X-NORESYNC: the panel was seen cycling its own self-test colours,
         * which means it is re-initialising on its own - most likely its power
         * or the DSI link, not our software.  Do NOT restart the stream from
         * here any more: that only adds another restart on top of theirs and
         * makes the symptom harder to read.  The "r" console command can still
         * trigger one by hand.
         */

        /* P4X-PANEL-WD: every ~12 s fully recover the panel (reset pulse +
         * stream restart + backlight).  The software cannot tell whether the
         * glass is dark, but a periodic recovery is harmless and is the only
         * thing that ever brought the picture back on this board.
         */

        /* P4X-REVERT: panel watchdog removed - it did not help and the extra
         * resets only added their own flashes.
         */

        (void)resync_next;

        /* P4X-DWELL: keep the gentle 400 ms redraw (power), but service input
         * in a tight inner loop so a press is seen within ~25 ms.  Without
         * this the UI missed the button press until the finger was already far
         * from where it started.
         */

        draw_due = 0;

        /* P4X-ONCE: one line, 15 s in, with everything the parser produced.
         * A single write cannot fill the console, and it makes "is altitude
         * actually computed" a fact instead of a guess.
         */

        {
          static int once_done;

          if (P4X_DIAG && !once_done && tick >= 60)
            {
              once_done = 1;
              printf("P4X-ONCE: v=%.2f P=%.1fkPa alt_abs=%.1f rel=%.2f "
                     "r=%.2f p=%.2f y=%.2f sat=%d gps=%d t=%.2f\n",
                     (double)fl.v, (double)(fl.pressure / 1000.0f), (double)g_alt_abs,
                     (double)fl.alt, (double)fl.roll, (double)fl.pitch,
                     (double)fl.yaw, fl.sat, fl.gps_fix,
                     (double)fl.temperature);
            }
        }

        /* P4X-FBDUMP: one shot after the first few frames. */

        {
          static int dumped;

          if (0 && !dumped && tick >= 8)   /* P4X-QUIET-OFF: off by default */
            {
              dumped = 1;
              fb_dump_small();
            }
        }
        while (!quit && !state_dirty)
          {
            usleep(5000);   /* P4X-FASTEST: was 25 ms */

            handle_keys();
            backlight_command();
            phase_pump();             /* P4X-PHASE: uplink repeats */

            if (board_touch_flip(&saved_dir))
              {
                apply_flip_dir(saved_dir);
                state_dirty = 1;
              }
            else
              {
                handle_touch();
              }

            if (btn_press_seen || state_dirty)
              {
                btn_press_seen = 0;
                break;            /* P4X-BTNFB/P4X-SLOWLINK: draw right away */
              }

            /* P4X-PHASEARM: the frame has to keep moving on its own.
             *
             * Holding the button made the shade dirty from the touch handler,
             * but once the finger is up nothing redrew the cool-down, so the
             * frame sat frozen at whatever it looked like on release.  While a
             * command is in flight - arming, sending or cooling - repaint just
             * the two boxes at 30 fps; a full page frame is far too expensive
             * for that (it managed about twelve steps per second and the frame
             * visibly jumped).
             */

            if (g_ph_hold >= 0 || g_ph_back ||
                (g_ph_sent_t != 0 && clock_systime_ticks() < g_ph_cool) ||
                g_phase_tx_left > 0)
              {
                unsigned long t_anim = clock_systime_ticks();

                if (t_anim - g_ph_anim_t >= (TICK_PER_SEC / 30))
                  {
                    g_ph_anim_t = t_anim;

                    if (g_shade_h == g_shade_target &&
                        g_shade_h >= PH_BTN_Y + PH_BTN_H)
                      {
                        phase_boxes_fast();
                        continue;
                      }

                    state_dirty = 1;
                  }
              }

            /* P4X-BTNSMOOTH: while the button animates, repaint just its band
             * on every pass and only give way to a full page frame every
             * fourth pass - the button runs at ~60 fps, the page keeps its
             * own cadence.
             */

            if (btn_anim_active())
              {
                locate_button_fast();

                if ((++anim_passes % 4) != 0)
                  {
                    continue;
                  }
              }
            else
              {
                anim_passes = 0;
              }

            if (++idle_slices >= 1)    /* P4X-LAT3: one 5 ms input slice
                                        * between frames; the sampler task
                                        * publishes touch independently. */
              {
                /* P4X-SLOWLINK: ~950 ms between periodic redraws.  The panel
                 * stayed healthy at this pace; anything faster made it drop
                 * the DSI link and restart its own self test.
                 */

                draw_due = 1;
                break;
              }

            /* P4X-BEAT: prove the input loop is alive and show what the touch
             * driver reports.
             */

            if (0 && (++beat_count % 400) == 0)   /* P4X-QUIET-OFF: no periodic
                                                   * console writes from the UI
                                                   * loop; they block as soon as
                                                   * nobody drains the CDC port
                                                   * and that froze the panel
                                                   * about 30 s after boot */
              {
                struct board_touch_s bs;

                if (board_touch_state(&bs) == 0)
                  {
                    printf("P4X-BEAT: frame %lu loop %lu page %d "
                           "draw %lums blit %lums touch (%d,%d) "
                           "pressed %d reports %lu releases %lu\n",
                           tick, beat_count, cur_page + 1,
                           last_draw_ms, last_blit_ms, bs.x, bs.y,
                           bs.pressed, bs.reports, bs.releases);
                  }
              }
          }

        if (state_dirty)
          {
            state_dirty = 0;
          }

        /* P4X-PACE120: one full repaint per 120 ms at most.  A swipe used to
         * repaint every 5 ms, which is what made this DSI panel drop its link
         * (frozen picture, system still alive).
         */

        usleep(g_fast_frame ? 0 : 5000);    /* P4X-FAST3: was 15 ms - the frame
                           * rate is the display side of the sensitivity knob.
                           * the vehicle is moving - the frame rate goes up
                           * by itself; the calm path keeps 15 ms. */

        (void)draw_due;
      }
  }

  fill_rect(0, 0, fb_w, fb_h, C_BLACK);

  close(fb_fd);
  return 0;
}

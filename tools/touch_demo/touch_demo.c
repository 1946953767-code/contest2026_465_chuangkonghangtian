/****************************************************************************
 * Touch test with LCD drawing - trajectory lines
 * - reads /dev/input0 (IRQ) AND polls /dev/i2c3 (bitbang)
 * - draws continuous trajectory lines on LCD (like pen drawing)
 ****************************************************************************/
#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/i2c/i2c_master.h>

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_CYAN    0x07FF
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_DARK    0x18A3
#define C_PEN     0x07FF  /* 青色轨迹线 */

#define FT6146_ADDR  0x38

static uint16_t *fb;
static int fb_w, fb_h, fb_stride;
static int fb_fd = -1;
static int i2c_fd = -1;

/* 轨迹记录 */
static int prev_x = -1, prev_y = -1;
static int count = 0;

/* 8x8 ASCII font */
static const uint8_t font8[128][8] = {
    [' ']={0,0,0,0,0,0,0,0},
    ['0']={0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},
    ['1']={0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
    ['2']={0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0},
    ['3']={0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},
    ['4']={0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},
    ['5']={0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},
    ['6']={0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},
    ['7']={0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},
    ['8']={0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},
    ['9']={0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},
    ['A']={0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},
    ['C']={0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},
    ['D']={0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0},
    ['E']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},
    ['H']={0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},
    ['I']={0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0},
    ['L']={0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},
    ['O']={0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},
    ['P']={0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},
    ['R']={0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0},
    ['S']={0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0},
    ['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},
    ['U']={0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},
    ['X']={0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},
    [':']={0,0x18,0x18,0,0x18,0x18,0},
    ['#']={0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0},
};

static void px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < fb_w && y >= 0 && y < fb_h)
        fb[y * fb_stride + x] = c;
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > fb_w) w = fb_w - x0;
    if (y0 + h > fb_h) h = fb_h - y0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            fb[y * fb_stride + x] = c;
}

static void draw_char(int x, int y, char ch, uint16_t c)
{
    int r, cc;
    if (ch < 0 || ch > 127) return;
    const uint8_t *g = font8[(int)ch];
    for (r = 0; r < 8; r++)
        for (cc = 0; cc < 8; cc++)
            if (g[r] & (0x80 >> cc))
                px(x + cc, y + r, c);
}

static void draw_text(int x, int y, const char *s, uint16_t c)
{
    while (*s) { draw_char(x, y, *s, c); x += 8; s++; }
}

static void fb_update_area(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    struct fb_area_s a = {x, y, w, h};
    ioctl(fb_fd, FBIO_UPDATE, &a);
}

static void fb_update_all(void)
{
    struct fb_area_s a = {0, 0, fb_w, fb_h};
    ioctl(fb_fd, FBIO_UPDATE, &a);
}

/* Bresenham 画线 - 两点间连线 (粗线: 中心+两侧) */
static void draw_line(int x0, int y0, int x1, int y1, uint16_t c, int thick)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int half = thick / 2;
    int t;

    while (1)
    {
        for (t = -half; t <= half; t++)
        {
            px(x0 + t, y0, c);
            px(x0, y0 + t, c);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 更新底部状态栏 */
static void update_status(int tx, int ty)
{
    char buf[64];
    fill_rect(0, fb_h - 24, fb_w, 24, C_DARK);
    snprintf(buf, sizeof(buf), "#%d x=%d y=%d", count, tx, ty);
    draw_text(4, fb_h - 18, buf, C_GREEN);
    draw_text(fb_w - 80, fb_h - 18, "TOUCH OK", C_CYAN);
    fb_update_area(0, fb_h - 24, fb_w, 24);
}

/* I2C 裸轮询读 FT6146 */
static int raw_read_touch(int *x, int *y)
{
    uint8_t reg = 0x02;
    uint8_t buf[5];
    struct i2c_msg_s msgs[2];
    struct i2c_transfer_s xfer;

    memset(buf, 0, sizeof(buf));
    msgs[0].frequency = 100000;
    msgs[0].addr = FT6146_ADDR;
    msgs[0].flags = 0;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;
    msgs[1].frequency = 100000;
    msgs[1].addr = FT6146_ADDR;
    msgs[1].flags = I2C_M_READ;
    msgs[1].buffer = buf;
    msgs[1].length = 5;
    xfer.msgv = msgs;
    xfer.msgc = 2;

    if (ioctl(i2c_fd, I2CIOC_TRANSFER, &xfer) < 0) return -1;

    if ((buf[0] & 0x0f) > 0)
    {
        *x = ((buf[1] & 0x0f) << 8) | buf[2];
        *y = ((buf[3] & 0x0f) << 8) | buf[4];
        return (buf[0] & 0x0f);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct touch_sample_s sample;
    struct fb_videoinfo_s vinfo;
    struct fb_planeinfo_s pinfo;
    int touch_fd;

    printf("=== TOUCH DEMO (trajectory) ===\n");

    /* LCD init */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { printf("ERR fb0: %d\n", errno); return 1; }
    ioctl(fb_fd, FBIOGET_VIDEOINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_PLANEINFO, &pinfo);
    fb_w = vinfo.xres;
    fb_h = vinfo.yres;
    fb_stride = pinfo.stride / 2;
    fb = (uint16_t *)mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FILE, fb_fd, 0);
    if (fb == MAP_FAILED) fb = (uint16_t *)pinfo.fbmem;
    printf("LCD: %dx%d\n", fb_w, fb_h);

    /* 初始画面 */
    fill_rect(0, 0, fb_w, fb_h, C_BLACK);
    fill_rect(0, 0, fb_w, 26, 0x0440);
    draw_text(4, 6, "TOUCH TEST", C_GREEN);
    draw_text(fb_w - 80, 6, "TEAM465", C_CYAN);
    fill_rect(0, 28, fb_w, 14, C_DARK);
    draw_text(4, 30, "Draw on screen!", C_YELLOW);

    /* CLEAR 按钮 (右侧) */
    fill_rect(fb_w - 60, 28, 58, 22, C_RED);
    draw_text(fb_w - 54, 33, "CLEAR", C_WHITE);

    update_status(0, 0);
    fb_update_all();

    /* 打开设备 */
    i2c_fd = open("/dev/i2c3", O_RDWR);
    touch_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
    printf("i2c_fd=%d touch_fd=%d\n", i2c_fd, touch_fd);

    printf("Drawing on screen...\n");

    while (1)
    {
        int tx = -1, ty = -1;
        int got = 0;

        /* 读 input0 (IRQ) */
        if (touch_fd >= 0)
        {
            int ret = read(touch_fd, &sample, sizeof(sample));
            if (ret > 0 && sample.npoints > 0)
            {
                tx = sample.point[0].x;
                ty = sample.point[0].y;
                got = 1;
            }
        }

        /* 裸轮询 I2C */
        if (!got && i2c_fd >= 0)
        {
            int n = raw_read_touch(&tx, &ty);
            if (n > 0) got = 1;
        }

        if (got && tx >= 0 && ty >= 0)
        {
            count++;

            /* 检测 CLEAR 按钮 (右上角) */
            if (tx > fb_w - 62 && ty >= 28 && ty <= 50)
            {
                /* 清除绘图区域 */
                fill_rect(0, 52, fb_w, fb_h - 76, C_BLACK);
                /* 重绘 CLEAR 按钮 */
                fill_rect(fb_w - 60, 28, 58, 22, C_RED);
                draw_text(fb_w - 54, 33, "CLEAR", C_WHITE);
                update_status(0, 0);
                fb_update_all();
                prev_x = -1;
                prev_y = -1;
                count = 0;
                printf("CLEARED\n");
                usleep(200000);
                continue;
            }

            /* 有前一个点 → 画轨迹线 (3px 粗, 青色) */
            if (prev_x >= 0 && prev_y >= 0)
            {
                draw_line(prev_x, prev_y, tx, ty, C_PEN, 3);
            }

            /* 画当前点 (小红点) */
            px(tx - 1, ty, C_RED);
            px(tx + 1, ty, C_RED);
            px(tx, ty - 1, C_RED);
            px(tx, ty + 1, C_RED);
            px(tx, ty, C_WHITE);

            /* 只刷新触摸点附近区域（不全屏刷！） */
            {
                int rx = (prev_x >= 0 && prev_x < tx) ? prev_x : tx;
                int ry = (prev_y >= 0 && prev_y < ty) ? prev_y : ty;
                int rw = (prev_x >= 0) ? abs(tx - prev_x) + 6 : 6;
                int rh = (prev_y >= 0) ? abs(ty - prev_y) + 6 : 6;
                if (rw < 10) rw = 10;
                if (rh < 10) rh = 10;
                fb_update_area(rx - 3, ry - 3, rw, rh);
            }

            /* 状态栏每 20 个事件才刷一次 */
            if (count % 20 == 0)
            {
                update_status(tx, ty);
                printf("TOUCH #%d x=%d y=%d\n", count, tx, ty);
            }

            prev_x = tx;
            prev_y = ty;
        }
        else
        {
            /* 手指抬起 */
            prev_x = -1;
            prev_y = -1;
        }

        usleep(5000); /* 5ms 轮询 */
    }

    return 0;
}

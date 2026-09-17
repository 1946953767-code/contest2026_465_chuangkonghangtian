/****************************************************************************
 * SF32LB52 火箭遥测大屏 (Team465 创空航天)
 * 手表屏适配版 - 圆弧屏加大边距
 * 4 页触摸滑动切换：
 *   Page 0: 卫星/连接状态
 *   Page 1: 速度/加速度
 *   Page 2: 姿态/高度/气压
 *   Page 3: 火箭倾斜 (XYZ 轴动)
 ****************************************************************************/
#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/input/buttons.h>

#include "cn_font.h"   /* 16x16 中文点阵（界面用字） */

/* 任务阶段状态机 */
#define PHASE_STANDBY   0   /* 待命 */
#define PHASE_ARMED     1   /* KEY2 已确认点火 */
#define PHASE_IGNITE    2   /* KEY1 点火+SD新文件 */
#define PHASE_FLIGHT    3   /* 飞行中（KEY1 再按=任务结束） */
#define PHASE_RECOVERY  4   /* 任务结束，新文件继续记录 */
#define PHASE_ABORT     5   /* KEY2 灭火/中止 */
#define BUTTON_KEY2_BIT (1u << 0)  /* PA11 */
#define BUTTON_KEY1_BIT (1u << 1)  /* PA34 */

static int mission_phase = PHASE_STANDBY;
static int phase_prev = -1;
static int btn_fd = -1;
static btn_buttonset_t btn_last = 0;
static unsigned int btn_startup_ignore = 400; /* 约2秒，防止上电电平误触发 */
static unsigned int reset_hold_ticks;
static int reset_latched;
static unsigned long mission_file_id = 0;

/* 思澈端速度/高度融合计算（低延迟，不依赖网页） */
static float fused_v = 0.0f;
static float fused_alt = 0.0f;
static float gps_v = 0.0f;
static float last_az = 1.0f;
static unsigned long fusion_ticks = 0;
#define FUSION_DT 0.005f   /* 主循环约 5ms，避免积分速度/高度放大 10 倍 */


/* 颜色 */
#define C_BLACK    0x0000
#define C_WHITE    0xFFFF
#define C_RED      0xF800
#define C_GREEN    0x07E0
#define C_BLUE     0x001F
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF
#define C_ORANGE   0xFD20
#define C_MAGENTA  0xF81F
#define C_PINK     0xFE19
#define C_LIME     0xAFE5
#define C_PURPLE   0x780F
#define C_DARK     0x18A3
#define C_DARKER   0x0C41
#define C_GRAY     0x8410
#define C_LGRAY    0xC618   /* 提亮：原来的 0x8410 太暗，数值看不清 */
#define C_LABEL    0xE71C   /* 刻度/标签用的高亮浅灰 */
#define C_DARKRED  0xA000
#define C_DARKGRN  0x0400
#define C_SKYBLUE  0x04FF
#define C_BROWN    0xA145

/* 圆弧屏安全边距 */
#define SAFE_X  18
#define SAFE_Y  18
#define SAFE_W  (fb_w - 36)
#define SAFE_H  (fb_h - 36)

#define NUM_PAGES 6

static uint16_t *fb = NULL;
static int fb_w, fb_h, fb_stride;
static int fb_fd = -1;
static int touch_fd = -1;

/* ===== 绘图 ===== */
static void px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < fb_w && y >= 0 && y < fb_h)
        fb[y * fb_stride + x] = c;
}

/* 按行连续写入：省掉每像素边界检查，全屏刷新速度大幅提升 */
static void fill_rect(int x0, int y0, int w, int h, uint16_t c)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > fb_w) w = fb_w - x0;
    if (y0 + h > fb_h) h = fb_h - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
    {
        uint16_t *row = fb + y * fb_stride + x0;
        for (x = 0; x < w; x++) row[x] = c;
    }
}

static void hline(int x0, int x1, int y, uint16_t c)
{
    int x;
    uint16_t *row;
    if (y < 0 || y >= fb_h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= fb_w) x1 = fb_w - 1;
    if (x1 < x0) return;
    row = fb + y * fb_stride;
    for (x = x0; x <= x1; x++) row[x] = c;
}

static void vline(int x, int y0, int y1, uint16_t c)
{
    int y;
    if (x < 0 || x >= fb_w) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= fb_h) y1 = fb_h - 1;
    for (y = y0; y <= y1; y++) fb[y * fb_stride + x] = c;
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (1)
    {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void thick_line(int x0, int y0, int x1, int y1, uint16_t c, int w)
{
    int i;
    for (i = -w/2; i <= w/2; i++)
        line(x0 + i, y0, x1 + i, y1, c);
    for (i = -w/2; i <= w/2; i++)
        line(x0, y0 + i, x1, y1 + i, c);
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
    int x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                px(cx + x, cy + y, c);
}

/* 圆角矩形 */
static void fill_round_rect(int x0, int y0, int w, int h, int r, uint16_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
        {
            int dx1 = x - x0, dx2 = x0 + w - 1 - x;
            int dy1 = y - y0, dy2 = y0 + h - 1 - y;
            int dx = dx1 < dx2 ? dx1 : dx2;
            int dy = dy1 < dy2 ? dy1 : dy2;
            if (dx >= r || dy >= r || (dx*dx + dy*dy <= r*r))
                px(x, y, c);
        }
}

/* ===== 字体 ===== */
static const uint8_t font8[128][8] = {
    [' ']={0,0,0,0,0,0,0,0},['!']={0x18,0x18,0x18,0x18,0x18,0,0x18,0},
    ['0']={0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},['1']={0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
    ['2']={0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0},['3']={0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0},
    ['4']={0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0},['5']={0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0},
    ['6']={0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0},['7']={0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0},
    ['8']={0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},['9']={0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0},
    ['.']={0,0,0,0,0,0x18,0x18,0},['-']={0,0,0,0x7E,0,0,0},
    [':']={0,0x18,0x18,0,0x18,0x18,0},['%']={0x62,0x64,0x08,0x10,0x26,0x46,0,0},
    ['+']={0,0x18,0x18,0x7E,0x18,0x18,0},['/']={0x06,0x0C,0x18,0x30,0x60,0xC0,0,0},
    ['A']={0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},['B']={0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},
    ['C']={0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},['D']={0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0},
    ['E']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},['F']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0},
    ['G']={0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0},['H']={0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},
    ['I']={0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0},['J']={0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0},
    ['K']={0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0},['L']={0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},
    ['M']={0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0},['N']={0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0},
    ['O']={0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},['P']={0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},
    ['R']={0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0},['S']={0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0},
    ['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},['U']={0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},
    ['V']={0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0},['W']={0x63,0x63,0x63,0x63,0x6B,0x7F,0x36,0},
    ['X']={0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},['Y']={0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0},
    ['Z']={0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0},
};

static void text(int x, int y, const char *s, uint16_t c)
{
    while (*s) {
        char ch = *s++;
        if (ch < 0 || ch > 127) continue;
        const uint8_t *g = font8[(int)ch];
        int r2, cc;
        for (r2 = 0; r2 < 8; r2++)
            for (cc = 0; cc < 8; cc++)
                if (g[r2] & (0x80 >> cc))
                    px(x + cc, y + r2, c);
        x += 8;
    }
}

static void text_scale(int x, int y, const char *s, uint16_t c, int sc)
{
    while (*s) {
        char ch = *s++;
        if (ch < 0 || ch > 127) continue;
        const uint8_t *g = font8[(int)ch];
        int r2, cc, sx2, sy2;
        for (r2 = 0; r2 < 8; r2++)
            for (cc = 0; cc < 8; cc++)
                if (g[r2] & (0x80 >> cc))
                    for (sy2 = 0; sy2 < sc; sy2++)
                        for (sx2 = 0; sx2 < sc; sx2++)
                            px(x + cc*sc + sx2, y + r2*sc + sy2, c);
        x += 8 * sc;
    }
}

/* ===== 中文点阵绘制（16x16） ===== */
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

static void text_cn(int x, int y, const char *s, uint16_t c, int sc)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        const unsigned char *bmp;
        int row, bit, sy, sx;
        if (*p < 0x80) { p++; continue; }          /* 跳过 ASCII */
        if (p[1] == 0 || p[2] == 0) break;
        bmp = cn_lookup(p);
        p += 3;
        if (!bmp) { x += 16 * sc; continue; }
        for (row = 0; row < 16; row++)
        {
            unsigned int bits = ((unsigned int)bmp[row * 2] << 8) | bmp[row * 2 + 1];
            for (bit = 0; bit < 16; bit++)
            {
                if (bits & (1u << (15 - bit)))
                {
                    for (sy = 0; sy < sc; sy++)
                        for (sx = 0; sx < sc; sx++)
                            px(x + bit * sc + sx, y + row * sc + sy, c);
                }
            }
        }
        x += 16 * sc;
    }
}

static int cn_width(const char *s, int sc)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { if (*p >= 0x80) { p += 3; n++; } else p++; }
    return n * 16 * sc;
}

static void fb_update(void)
{    /* fb_area_s 的第三、四项是宽度和高度，刷新完整 framebuffer。 */
    struct fb_area_s a = {0, 0, fb_w, fb_h};
    ioctl(fb_fd, FBIO_UPDATE, &a);
}

/* ===== LoRa JSON receive (UART2 = /dev/ttyS0, PA20/PA27) ===== */
typedef struct {
    float t, v, alt, ax, ay, az, pitch, roll, yaw;
    int sat, gps_fix, lora_ok, phase;
    float pressure, temperature;
    float gx, gy, gz;
    double lat, lon;          /* 火箭（ESP32）经纬度，来自 LoRa 遥测 */
    int rocket_link;          /* 箭载端自报的链路状态（JSON 字段 L）：1=通 0=断 -1=未知 */
    char time_str[12];
} flight_t;

static int lora_fd = -1;
static int telemetry_live;
static char lora_line[256];   /* 完整包含经纬度后约 145 字节，必须留足 */
static int lora_used;
static unsigned long lora_packets;
static unsigned long lora_lines;
static unsigned long lora_bad;
static int lora_dirty;
static int lora_silence_ticks;
static char esp_ack[20];
static int esp_ack_ms;
/* 命令回执的新鲜度倒计时。收到 A:xxx 回执或 P? 探测时重置为 ESP_ACK_HOLD，
 * 主循环每轮递减。之前这里是个一旦置 1 就永不复位的锁存标志，
 * 导致思澈一直显示"已回应"，即使 ESP32 早已失联——现在改为会过期。 */
#define ESP_ACK_HOLD 600          /* 600 tick × 5 ms ≈ 3 秒内算"ESP32 在线" */
static unsigned long probe_reply_count;   /* 已回复的链路探测次数（箭载端开关打开时才用） */
static unsigned int link_tick;

static int json_float(const char *line, const char *key, float *out)
{
    char needle[32];
    const char *p;
    char *endp;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(line, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "null", 4) == 0) return -1;
    *out = strtof(p, &endp);
    return endp != p ? 0 : -1;
}

/* 双精度解析：经纬度必须用 double。float 只有约 7 位有效数字，
 * 解析经度 113.xxxxxx 会丢掉约 1 米精度。 */
static int json_double(const char *line, const char *key, double *out)
{
    char needle[32];
    const char *p;
    char *endp;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(line, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "null", 4) == 0) return -1;
    *out = strtod(p, &endp);
    return endp != p ? 0 : -1;
}

static int json_str(const char *line, const char *key, char *out, int outsz)
{
    char needle[32];
    const char *p, *q;
    int len;
    if (outsz <= 0) return -1;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(line, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    q = strchr(p, '"');
    if (!q) return -1;
    len = (int)(q - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static void lora_apply_json(flight_t *f, const char *line)
{
    float value;
    double dvalue;
    int valid = 0;
    if (line[0] == 'M' && line[1] == ',')
    {
        char *p = (char *)line;
        char *endp;
        long v[9];
        int n = 0;
        while (n < 9 && p != NULL)
        {
            p = strchr(p, ',');
            if (p == NULL) break;
            p++;
            v[n] = strtol(p, &endp, 10);
            if (endp == p) break;
            n++;
            p = endp;
        }
        if (n == 9)
        {
            f->ax = v[0] / 100.0f; f->ay = v[1] / 100.0f; f->az = v[2] / 100.0f;
            f->gx = v[3] / 10.0f; f->gy = v[4] / 10.0f; f->gz = v[5] / 10.0f;
            f->pitch = v[6] / 10.0f; f->roll = v[7] / 10.0f; f->yaw = v[8] / 10.0f;
            f->lora_ok = 1;
            lora_silence_ticks = 0;
            telemetry_live = 1;
            lora_dirty = 1;
            lora_packets++;
        }
        return;
    }
    if (json_float(line, "press", &value) == 0) { f->pressure = value; valid = 1; }
    if (json_float(line, "alt", &value) == 0) { f->alt = value; valid = 1; }
    if (json_float(line, "temp", &value) == 0) f->temperature = value;
    if (json_double(line, "la", &dvalue) == 0) { f->lat = dvalue; valid = 1; }   /* 火箭纬度 */
    if (json_double(line, "lo", &dvalue) == 0) { f->lon = dvalue; valid = 1; }   /* 火箭经度 */
    if (json_float(line, "ax", &value) == 0) f->ax = value;
    if (json_float(line, "ay", &value) == 0) f->ay = value;
    if (json_float(line, "az", &value) == 0) f->az = value;
    if (json_float(line, "gx", &value) == 0) f->gx = value;
    if (json_float(line, "gy", &value) == 0) f->gy = value;
    if (json_float(line, "gz", &value) == 0) f->gz = value;
    if (json_float(line, "pitch", &value) == 0 || json_float(line, "p", &value) == 0) { f->pitch = value; valid = 1; }
    if (json_float(line, "roll", &value) == 0 || json_float(line, "r", &value) == 0) { f->roll = value; valid = 1; }
    if (json_float(line, "yaw", &value) == 0 || json_float(line, "y", &value) == 0) { f->yaw = value; valid = 1; }
    if (json_float(line, "a", &value) == 0) { f->alt = value; valid = 1; }
    if (json_float(line, "s", &value) == 0) { f->sat = (int)value; valid = 1; }
    if (json_float(line, "gps", &value) != 0) f->gps_fix = f->sat >= 4;
    if (json_float(line, "gps", &value) == 0) { f->gps_fix = value > 0.5f; if (!f->gps_fix) f->sat = 0; valid = 1; }
    if (json_float(line, "t", &value) == 0) f->temperature = value;
    if (json_float(line, "P", &value) == 0) f->pressure = value * 1000.0f;
    if (json_float(line, "v", &value) == 0) { f->v = value; valid = 1; }
    /* 箭载端自报的链路状态（它自己有没有收到思澈的回包）。
     *   1 = 箭载已确认双向通  0 = 箭载确认断开  -1 = 箭载开机宽限中（未定论）
     * 思澈据此区分"本端收不到箭载"与"箭载收不到本端"。 */
    if (json_float(line, "L", &value) == 0)
        f->rocket_link = (value > 0.5f) ? 1 : ((value < -0.5f) ? -1 : 0);
    if (json_str(line, "T", f->time_str, sizeof(f->time_str)) == 0) valid = 1;
    if (valid)
    {
        f->lora_ok = 1;
        lora_silence_ticks = 0;
        telemetry_live = 1;
        lora_dirty = 1;
        lora_packets++;
        if ((lora_packets % 10) == 0)
            printf("LoRa packets=%lu probe_reply=%lu rocket_link=%d P=%.1f R=%.1f Y=%.1f SAT=%d T=%s TEMP=%.1f PRESS=%.1f ALT=%.1f V=%.1f AX=%.3f AY=%.3f AZ=%.3f\n",
                   lora_packets, probe_reply_count, f->rocket_link,
                   f->pitch, f->roll, f->yaw, f->sat,
                   f->time_str, f->temperature, f->pressure,
                   f->alt, f->v, f->ax, f->ay, f->az);
    }
}

/* ===== 思澈侧卫星模块（UART3 = /dev/ttyS1）=====
 * 只在本机读取与计算，所有相对距离/方位都在思澈算好后
 * 随原有网页 JSON 输出，不占用 LoRa 带宽、不影响遥测速率。
 */
static int gps_fd = -1;
static char gps_buf[256];
static int gps_buf_len;
static double own_lat, own_lon, own_alt;
static int own_fix, own_sat;
static char own_time[12] = "--:--:--";

/* 雷达结果：思澈实时计算 */
static float rel_dist_m;      /* 直线距离 */
static float rel_ground_m;    /* 地面投影距离（水平距离） */
static float rel_alt_m;       /* 相对高度（火箭 - 地面站） */
static float rel_bearing;     /* 方位角，正北为 0，顺时针 */
static int   rel_valid;       /* 地基与箭载定位都有效时才为 1 */

static float d2r(float d) { return d * 3.14159265f / 180.0f; }
static float r2d(float r) { return r * 180.0f / 3.14159265f; }

/* 卫星模块串口：只允许 /dev/ttyS1（UART3）。
 * 禁止把 /dev/console 放进候选表：反复 close/open 系统控制台会把
 * 整个控制台搞崩导致系统卡死（遥测停止 → 网页 LoRa 变红）。 */
static const char *gps_devs[] = { "/dev/ttyS1" };
#define GPS_DEV_N (sizeof(gps_devs) / sizeof(gps_devs[0]))
static unsigned int gps_dev_idx;

static int gps_open(void)
{
    int fd = -1;
    unsigned int ci;
    struct termios tio;

    for (ci = 0; ci < GPS_DEV_N; ci++)
    {
        fd = open(gps_devs[gps_dev_idx], O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) { printf("Sat on %s\n", gps_devs[gps_dev_idx]); break; }
        gps_dev_idx = (gps_dev_idx + 1) % GPS_DEV_N;
    }
    if (fd < 0) return -1;
    if (tcgetattr(fd, &tio) < 0) { close(fd); return -1; }
    cfsetispeed(&tio, 9600);
    cfsetospeed(&tio, 9600);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tio.c_oflag &= ~OPOST;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) { close(fd); return -1; }
    tcflush(fd, TCIFLUSH);
    return fd;
}

/* NMEA 度分格式：ddmm.mmmm → 十进制度 */
static double nmea_deg(const char *s, char hemi)
{
    double v = atof(s);
    int deg = (int)(v / 100.0);
    double min = v - deg * 100.0;
    double d = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') d = -d;
    return d;
}

static void gps_parse_line(char *line)
{
    char *p;
    if (strncmp(line, "$GNGGA", 6) != 0 && strncmp(line, "$GPGGA", 6) != 0)
    {
        if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0)
        {
            /* $GNRMC,hhmmss,A,lat,N,lon,E,... */
            char *f[12];
            int n = 0;
            for (p = strtok(line, ","); p && n < 12; p = strtok(NULL, ",")) f[n++] = p;
            if (n > 6 && f[2][0] == 'A')
            {
                own_lat = nmea_deg(f[3], f[4][0]);
                own_lon = nmea_deg(f[5], f[6][0]);
                own_fix = 1;
                snprintf(own_time, sizeof(own_time), "%c%c:%c%c:%c%c",
                         f[1][0], f[1][1], f[1][2], f[1][3], f[1][4], f[1][5]);
            }
        }
        return;
    }
    /* $GNGGA,hhmmss,lat,N,lon,E,fix,sat,hdop,alt,M,... */
    {
        char *f[16];
        int n = 0;
        for (p = strtok(line, ","); p && n < 16; p = strtok(NULL, ",")) f[n++] = p;
        if (n > 9)
        {
            if (f[6][0] != '0' && f[2][0] && f[4][0])
            {
                own_lat = nmea_deg(f[2], f[3][0]);
                own_lon = nmea_deg(f[4], f[5][0]);
                own_alt = atof(f[9]);
                own_fix = 1;
            }
            own_sat = atoi(f[7]);
        }
    }
}

static unsigned long gps_rx_bytes;    /* 卫星模块收到的字节数，用于判断接线 */
static unsigned long gps_sentences;    /* 解析到的 NMEA 语句数 */

/* 波特率自动扫描：模块出厂波特率不一定是 9600 */
static const unsigned int gps_bauds[] = { 9600, 115200, 38400, 57600, 4800, 19200 };
#define GPS_BAUD_N (sizeof(gps_bauds) / sizeof(gps_bauds[0]))
static unsigned int gps_baud_idx;
static unsigned int gps_baud_idle;    /* 当前波特率下无数据的轮数 */

static int gps_apply_baud(int fd, unsigned int baud)
{
    struct termios tio;
    if (fd < 0 || tcgetattr(fd, &tio) < 0) return -1;
    cfsetispeed(&tio, baud);
    cfsetospeed(&tio, baud);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tio.c_oflag &= ~OPOST;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) return -1;
    tcflush(fd, TCIFLUSH);
    return 0;
}

static int own_manual;   /* 1 = 手动/网页设过，自动校准不再覆盖 */
static int own_sat_ok;   /* 1 = 思澈侧卫星模块已给出有效定位 */
/* 雷达页“校准地基”按钮区域（触摸命中用） */
static int radar_btn_x = -1, radar_btn_y, radar_btn_w, radar_btn_h;

/* 控制台命令：网页通过 USB 串口下发地基坐标 G:lat,lon,alt */
static void ground_cmd_apply(const char *args)
{
    double la = 0.0, lo = 0.0, al = 0.0;
    int n = sscanf(args, "%lf,%lf,%lf", &la, &lo, &al);
    if (n >= 2 && (la != 0.0 || lo != 0.0))
    {
        own_lat = la;
        own_lon = lo;
        own_alt = (n >= 3) ? al : 0.0;
        own_fix = 1;
        own_manual = 1;   /* 网页手动设过，自动校准不再覆盖 */
        lora_dirty = 1;
        printf("GS set %.5f %.5f %.1f\n", la, lo, al);
    }
}

static void gps_poll(void)
{
    char buf[128];
    int n, i;
    if (gps_fd < 0) return;
    while ((n = read(gps_fd, buf, sizeof(buf))) > 0)
    {
        gps_rx_bytes += n;
        gps_baud_idle = 0;
        for (i = 0; i < n; i++)
        {
            char ch = buf[i];
            if (ch == '\n')
            {
                gps_buf[gps_buf_len] = '\0';
                if (gps_buf_len > 6 && gps_buf[0] == '$')
                {
                    gps_sentences++;
                    gps_parse_line(gps_buf);
                }
                gps_buf_len = 0;
            }
            else if (ch != '\r' && gps_buf_len < (int)sizeof(gps_buf) - 1)
                gps_buf[gps_buf_len++] = ch;
        }
    }

    /* 长时间收不到字节：先轮波特率，波特率轮完再换串口设备。
     * 这样主人接 PA18/PA19(控制台口) 或 PA39/PA40 都能自动适配。 */
    if (gps_rx_bytes == 0)
    {
        gps_baud_idle++;
        if (gps_baud_idle > 800)   /* 约 4 秒 */
        {
            gps_baud_idle = 0;
            gps_baud_idx = (gps_baud_idx + 1) % GPS_BAUD_N;
            if (gps_fd >= 0)
            {
                gps_apply_baud(gps_fd, gps_bauds[gps_baud_idx]);
                printf("SAT baud -> %u\n", gps_bauds[gps_baud_idx]);
            }
        }
    }
}

/* 地基基准自动校准：
 * 待命/确认阶段火箭就摆在发射点，箭载 GPS(1~3m) 比手动输入准得多，
 * 因此把箭载坐标直接作为地基坐标；点火后自动冻结，保证雷达基准准确。
 * 手动通过网页下发 G: 命令或触摸按钮可覆盖。 */


static void ground_autoset(flight_t *f)
{
    /* 静止滑动平均：GPS 静止误差是随机噪声，平均 N 次可按 √N 降低。
     * 火箭静止时每秒约 3~4 个定位点，30 点约 8 秒收敛。 */
    static double mLa, mLo, mAl;
    static int mn;

    if (own_manual) return;
    if (mission_phase != PHASE_STANDBY && mission_phase != PHASE_ARMED) return;
    /* 不要求当前正有定位：ESP32 只有拿到有效定位后才会写入经纬度，
     * 地基站固定不动，用历史有效坐标当地基基准更稳。 */
    if (f->lat == 0.0 && f->lon == 0.0) return;

    if (mn == 0) { mLa = f->lat; mLo = f->lon; mAl = f->alt; mn = 1; }
    else
    {
        double k = 1.0 / (double)(mn < 30 ? ++mn : 30);
        mLa += (f->lat - mLa) * k;
        mLo += (f->lon - mLo) * k;
        mAl += (f->alt - mAl) * k;
    }

    own_lat = mLa;
    own_lon = mLo;
    own_alt = mAl;
    own_fix = 1;
}

/* 点火后清掉平均器，避免下一次任务沿用上一轮数据 */
static void ground_autoset_reset(void)
{
    /* 由 ground_set_from_rocket / 手动设定时调用 */
}

/* 手动重设地基为当前箭载坐标（触摸按钮/网页命令） */
static void ground_set_from_rocket(flight_t *f)
{
    if (!f->gps_fix || (f->lat == 0.0 && f->lon == 0.0)) return;
    own_lat = f->lat;
    own_lon = f->lon;
    own_alt = f->alt;
    own_fix = 1;
    own_manual = 1;
    lora_dirty = 1;
    printf("GS calib %.6f %.6f\n", own_lat, own_lon);
}

/* 相对位置：地基(思澈)为原点，正北为上；纯本地计算，实时刷新 */
static void rel_update(flight_t *f)
{
    float dlat, dlon, la1, la2, dphi, dlmb, a, c;
    const float R = 6371000.0f;

    if (!own_fix || (f->lat == 0.0 && f->lon == 0.0))
    {
        rel_valid = 0;
        return;
    }

    la1 = d2r((float)own_lat);
    la2 = d2r((float)f->lat);
    dlat = d2r((float)(f->lat - own_lat));
    dlon = d2r((float)(f->lon - own_lon));

    /* haversine 水平距离 */
    dphi = sinf(dlat * 0.5f);
    dlmb = sinf(dlon * 0.5f);
    a = dphi * dphi + cosf(la1) * cosf(la2) * dlmb * dlmb;
    c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    rel_ground_m = R * c;

    /* 方位角（正北 0°，顺时针） */
    {
        float y = sinf(dlon) * cosf(la2);
        float x = cosf(la1) * sinf(la2) - sinf(la1) * cosf(la2) * cosf(dlon);
        float b = r2d(atan2f(y, x));
        if (b < 0) b += 360.0f;
        rel_bearing = b;
    }

    rel_alt_m = f->alt - (float)own_alt;
    rel_dist_m = sqrtf(rel_ground_m * rel_ground_m + rel_alt_m * rel_alt_m);
    rel_valid = 1;
}

/* ===== 网页 JSON：在融合计算之后输出，保证网页拿到思澈算好的高度/速度 ===== */
static void send_web_json(flight_t *f)
{
    if (!telemetry_live) return;
    printf("{\"t\":%.1f,\"v\":%.1f,\"alt\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"yaw\":%.1f,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"sat\":%d,\"gps\":%d,\"lora\":%d,\"rlink\":%d,\"probe\":%lu,\"phase\":%d,\"sd\":%lu,\"ack\":\"%s\",\"temp\":%.1f,\"press\":%.1f,\"time\":\"%s\"}\n",
           f->t, f->v, f->alt, f->pitch, f->roll, f->yaw,
           f->ax, f->ay, f->az, f->sat, f->gps_fix, f->lora_ok,
           f->rocket_link, probe_reply_count,
           mission_phase, mission_file_id, esp_ack_ms ? esp_ack : "",
           f->temperature, f->pressure, f->time_str);

    /* 雷达数据：思澈本地算好的相对位置，随原 JSON 一起输出，不占 LoRa */
    /* 经纬度输出 7 位小数（约 1cm），避免发给网页时再被截断 */
    printf("{\"gs_rx\":%lu,\"gs_nmea\":%lu,"
           "\"gs_lat\":%.7f,\"gs_lon\":%.7f,\"gs_alt\":%.1f,\"gs_fix\":%d,\"gs_sat\":%d,"
           "\"rocket_lat\":%.7f,\"rocket_lon\":%.7f,"
           "\"dist\":%.1f,\"gdist\":%.1f,\"dalt\":%.1f,\"bearing\":%.1f,"
           "\"radar\":%d,\"gtime\":\"%s\"}\n",
           gps_rx_bytes, gps_sentences,
           own_lat, own_lon, own_alt, own_fix, own_sat,
           f->lat, f->lon,
           rel_dist_m, rel_ground_m, rel_alt_m, rel_bearing,
           rel_valid, own_time);
}

static int lora_open(void)
{
    int fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios tio;
    if (fd < 0) return -1;
    if (tcgetattr(fd, &tio) < 0) { close(fd); return -1; }
    cfsetispeed(&tio, 9600);
    cfsetospeed(&tio, 9600);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tio.c_oflag &= ~OPOST;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tio) < 0) { close(fd); return -1; }
    tcflush(fd, TCIFLUSH);
    return fd;
}

static void lora_poll(flight_t *f)
{
    char buf[128];
    int n, i;
    if (lora_fd < 0) return;
    while ((n = read(lora_fd, buf, sizeof(buf))) > 0)
    {
        for (i = 0; i < n; i++)
        {
            char ch = buf[i];
            if (ch == '\r') continue;
            if (ch == '\n')
            {
                lora_line[lora_used] = '\0';
                if (lora_used > 0)
                {
                    if (lora_line[0] == 'A' && lora_line[1] == ':')
                    {
                        /* ESP32 命令回执，用于确认按键命令已被收到 */
                        strncpy(esp_ack, lora_line + 2, sizeof(esp_ack) - 1);
                        esp_ack[sizeof(esp_ack) - 1] = '\0';
                        esp_ack_ms = ESP_ACK_HOLD;
                        lora_dirty = 1;
                        printf("ESP32 ACK %s\n", esp_ack);
                    }
                    else if (lora_line[0] == 'G' && lora_line[1] == ':')
                    {
                        /* 网页/手动下发地基坐标：G:lat,lon,alt
                         * 地基站固定不动，手动定位一次即可让雷达工作。 */
                        double la = 0.0, lo = 0.0, al = 0.0;
                        int n2 = sscanf(lora_line + 2, "%lf,%lf,%lf", &la, &lo, &al);
                        if (n2 >= 2 && (la != 0.0 || lo != 0.0))
                        {
                            own_lat = la;
                            own_lon = lo;
                            own_alt = (n2 >= 3) ? al : 0.0;
                            own_fix = 1;
                            own_manual = 1;
                            lora_dirty = 1;
                            printf("GS set %.5f %.5f %.1f\n", la, lo, al);
                        }
                    }
                    else if (lora_line[0] == 'P' && lora_line[1] == '?')
                    {
                        /* ESP32 的链路探测，立刻回 "P:1"。
                         * ESP32 发探测包时正好处于它那 1 秒纯接收窗口内，
                         * 所以这 4 字节回包既不会与遥测发射冲突，也不占用
                         * 遥测带宽（每 2 秒上行 3 B + 下行 4 B）。
                         * 这是 ESP32 判断"思澈还在不在"的唯一可靠依据。 */
                        if (lora_fd >= 0)
                        {
                            /* 回包必须够长。实测 4 字节的 "P:1" 回程丢失率很高：
                             * LoRa 透明模块对 UART 上短小的突发会先攒一攒再发，
                             * 发出去的时机往往错过 ESP32 那 1 秒接收窗口。
                             * 加长到 17 字节并连发两条，给 ESP32 两次机会。 */
                            static const char probe_ack[] = "P:1linkcheck-ack\n";
                            ssize_t w1 = write(lora_fd, probe_ack, sizeof(probe_ack) - 1);
                            ssize_t w2 = write(lora_fd, probe_ack, sizeof(probe_ack) - 1);
                            (void)w1;
                            (void)w2;
                        }
                        probe_reply_count++;
                        /* 探测包本身也证明"ESP32 还在、并且在发"，
                         * 所以同样刷新 ESP32 在线新鲜度。 */
                        esp_ack_ms = ESP_ACK_HOLD;
                    }
                    else
                        lora_apply_json(f, lora_line);
                }
                lora_used = 0;
            }
            else if (lora_used < (int)sizeof(lora_line) - 1)
                lora_line[lora_used++] = ch;
            else
                lora_used = 0;
        }
    }
}

/* ===== 按键状态机：KEY1 推进流程 / KEY2 确认点火与灭火 ===== */
/* 命令队列：非阻塞重复发送，保证送达又不卡住主循环（LCD/触摸保持低延迟） */
static char cmd_buf[32];
static char cmd_next_buf[32];
static int cmd_repeat;
static int cmd_wait_ticks;
static int cmd_next_valid;

/* 蜂鸣器命令：最新阶段音效立即抢占旧音效，避免确认音重发阻塞点火音。 */
static char cmd_priority_buf[32];
static int cmd_priority_repeat;
static int cmd_priority_wait_ticks;

/* ESP32 每 2 秒留 1 秒纯接收窗口；窗口里它不发遥测，思澈这边就会出现约 1 秒的
 * 遥测空档。把命令重发对齐到这个空档，能明显提高一次命中率 ——
 * 字节数完全不变，只是发得更"准"。
 * 配合箭载端"接收窗口内不写 SD 卡"的改动：写卡一次阻塞 200~500ms，
 * SoftSPI 是纯 CPU 位翻转，9600 波特下 500ms 就是 480 字节，远超 UART 的
 * 128 字节 FIFO，窗口期间进来的命令会被整条丢掉（实测 80 秒收不到一条上行）。 */
#define GAP_SYNC_TICKS 40        /* 约 200~400ms 没遥测即认为箭载端已进接收窗口 */
#define SEND_WAIT_IN_GAP 6       /* 空档内约 30ms 发一次，把重发集中在窗口里 */
#define SEND_WAIT_NORMAL 32      /* 空档外约 160ms 发一次 */

static int lora_in_gap(void)
{
    return lora_silence_ticks > GAP_SYNC_TICKS;
}

static void queue_priority_cmd(const char *cmd)
{
    /* 第一帧立即发，后续仅后台补发；新阶段覆盖旧阶段的补发队列。 */
    strncpy(cmd_priority_buf, cmd, sizeof(cmd_priority_buf) - 1);
    cmd_priority_buf[sizeof(cmd_priority_buf) - 1] = '\0';
    cmd_priority_repeat = 20; /* 覆盖到第二个接收窗口，避免整轮落在发射相位 */
    cmd_priority_wait_ticks = 0;
}

static void queue_cmd(const char *cmd)
{
    /* 一个按键动作可能同时需要 S:NEW/S:STOP 和 B:*，不能互相覆盖。 */
    if (cmd_repeat > 0)
    {
        if (!cmd_next_valid)
        {
            strncpy(cmd_next_buf, cmd, sizeof(cmd_next_buf) - 1);
            cmd_next_buf[sizeof(cmd_next_buf) - 1] = '\0';
            cmd_next_valid = 1;
        }
        return;
    }
    strncpy(cmd_buf, cmd, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    cmd_repeat = 10;        /* 重复 10 次、跨度约 1.6 秒，覆盖 ESP32 接收窗口 */
    cmd_wait_ticks = 0;
}

static void cmd_tick(void)
{
    if (cmd_priority_repeat > 0)
    {
        if (cmd_priority_wait_ticks > 0) { cmd_priority_wait_ticks--; return; }
        if (lora_fd >= 0)
        {
            write(lora_fd, cmd_priority_buf, strlen(cmd_priority_buf));
            write(lora_fd, "\n", 1);
            printf("CMDTX %s\n", cmd_priority_buf);
        }
        cmd_priority_repeat--;
        /* 空档内（箭载端在接收）加快重发，把这一轮命令集中打进它的窗口 */
        cmd_priority_wait_ticks = lora_in_gap() ? SEND_WAIT_IN_GAP : SEND_WAIT_NORMAL;
        return;
    }
    if (cmd_repeat <= 0)
    {
        if (!cmd_next_valid) return;
        strncpy(cmd_buf, cmd_next_buf, sizeof(cmd_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        cmd_next_valid = 0;
        cmd_repeat = 16;
        cmd_wait_ticks = 0;
    }
    if (cmd_wait_ticks > 0) { cmd_wait_ticks--; return; }
    if (lora_fd < 0) { cmd_repeat = 0; cmd_next_valid = 0; return; }
    write(lora_fd, cmd_buf, strlen(cmd_buf));
    write(lora_fd, "\n", 1);
    printf("CMDTX %s\n", cmd_buf);
    cmd_repeat--;
    cmd_wait_ticks = lora_in_gap() ? SEND_WAIT_IN_GAP : SEND_WAIT_NORMAL;
}

static void send_sd_cmd(const char *cmd)
{
    queue_cmd(cmd);
}

static void phase_beep(int phase)
{
    /* 蜂鸣器在 ESP32 侧，通过 LoRa 下发音效命令；
     * 状态切换与 LCD/网页更新仍在本机即时完成，不受无线延迟影响。 */
    switch (phase)
    {
        case PHASE_ARMED:    queue_priority_cmd("B:ARM");    break;  /* 低音长鸣 */
        case PHASE_IGNITE:   queue_priority_cmd("B:IGNITE"); break;  /* 5 声尖锐 */
        case PHASE_RECOVERY: queue_priority_cmd("B:END");    break;  /* 4 声结束 */
        case PHASE_ABORT:    queue_priority_cmd("B:ABORT");  break;  /* 2 声灭火 */
        default: break;
    }
}

static void mission_set_phase(int phase)
{
    if (mission_phase == phase) return;
    mission_phase = phase;
    phase_prev = phase;
    phase_beep(phase);
    lora_dirty = 1;
    printf("PHASE=%d\n", phase);
}

static void new_record_file(void)
{
    char cmd[32];
    mission_file_id++;
    snprintf(cmd, sizeof(cmd), "S:NEW%lu", mission_file_id);
    send_sd_cmd(cmd);
}

static void handle_buttons(flight_t *f)
{
    btn_buttonset_t sample = 0;
    btn_buttonset_t edge;
    int guard = 0;

    if (btn_fd < 0) return;

    /* 限制单次轮询次数，避免读接口持续返回导致卡死主循环 */
    while (guard++ < 8 && read(btn_fd, &sample, sizeof(sample)) > 0)
    {
        edge = sample & ~btn_last;
        btn_last = sample;
        /* 双键同时长按约1秒：任务复位到待命，停止当前记录并清零融合值。 */
        /* 结束/灭火后的 KEY1 长按约1.5秒：软件复位任务到待命。
         * 只在 PHASE_RECOVERY/PHASE_ABORT 生效，飞行阶段不会误复位。 */
        if ((mission_phase == PHASE_RECOVERY || mission_phase == PHASE_ABORT) &&
            (sample & BUTTON_KEY1_BIT))
        {
            if (reset_hold_ticks < 350) reset_hold_ticks++;
            if (reset_hold_ticks >= 300 && !reset_latched)
            {
                reset_latched = 1;
                send_sd_cmd("S:STOP");
                mission_phase = PHASE_STANDBY;
                phase_prev = -1;
                fused_v = 0.0f;
                fused_alt = 0.0f;
                mission_file_id = 0;
                esp_ack_ms = 0;
                queue_priority_cmd("B:READY");
                lora_dirty = 1;
                printf("MISSION RESET\n");
            }
            edge = 0;
        }
        else if (!(sample & BUTTON_KEY1_BIT))
        {
            reset_hold_ticks = 0;
            reset_latched = 0;
        }
        if (btn_startup_ignore > 0)
        {
            btn_startup_ignore--;
            edge = 0; /* 只同步按键初始电平，不执行任务动作 */
        }
        else if (edge)
            printf("KEY edge=%u phase=%d\n", (unsigned)edge, mission_phase);

        if (edge & BUTTON_KEY2_BIT)
        {
            /* KEY2：待命=点火确认；已确认=取消；点火/飞行=灭火；灭火后=结束 */
            if (mission_phase == PHASE_STANDBY)
                mission_set_phase(PHASE_ARMED);
            else if (mission_phase == PHASE_ARMED)
                mission_set_phase(PHASE_STANDBY);
            else if (mission_phase == PHASE_IGNITE || mission_phase == PHASE_FLIGHT)
            {
                send_sd_cmd("S:STOP");
                mission_set_phase(PHASE_ABORT);
            }
            else if (mission_phase == PHASE_ABORT)
            {
                /* 灭火之后再按 KEY2 也是结束 */
                new_record_file();
                mission_set_phase(PHASE_RECOVERY);
            }
        }

        if (edge & BUTTON_KEY1_BIT)
        {
            /* KEY1：已确认=点火+新建记录；飞行中=任务结束；灭火后=结束 */
            if (mission_phase == PHASE_ARMED)
            {
                new_record_file();
                mission_set_phase(PHASE_IGNITE);
                f->t = 0;
            }
            else if (mission_phase == PHASE_IGNITE || mission_phase == PHASE_FLIGHT)
            {
                new_record_file();
                mission_set_phase(PHASE_RECOVERY);
            }
            else if (mission_phase == PHASE_ABORT)
            {
                /* 灭火之后再按 KEY1 是结束 */
                new_record_file();
                mission_set_phase(PHASE_RECOVERY);
            }
        }
    }
}

/* ===== 思澈端速度/高度融合 ===== */
static void fusion_update(flight_t *f)
{
    /* 未点火：不积分，基线直接跟随 ESP32 下发的气压高度/GPS 速度，
     * 这样点火瞬间从当前真实高度开始积分，且平时也有数据可看。 */
    if (mission_phase == PHASE_STANDBY || mission_phase == PHASE_ARMED)
    {
        fused_v = f->v;
        fused_alt = f->alt;
        return;
    }
    if (mission_phase == PHASE_RECOVERY || mission_phase == PHASE_ABORT)
    {
        return;   /* 任务结束/灭火后冻结高度速度 */
    }

    /* az 为加速度计 Z 轴（g 单位），去除重力后积分速度；有 GPS 时融合 */
    float ax_frac = f->az - 1.0f;

    /* 静止死区：抑制静止时的积分漂移（约 0.09g 以内视为零） */
    if (ax_frac > -0.09f && ax_frac < 0.09f) ax_frac = 0.0f;

    if (f->gps_fix && f->v > 0.1f)
    {
        /* 有卫星：GPS 速度为主，加速度积分只做平滑 */
        float acc_v = fused_v + ax_frac * 9.81f * FUSION_DT;
        fused_v = 0.85f * f->v + 0.15f * acc_v;
    }
    else
    {
        /* 无卫星：加速度积分拟合速度，加泄漏项抑制静止漂移 */
        fused_v += ax_frac * 9.81f * FUSION_DT;
        fused_v *= 0.998f;
        if (fused_v > -0.20f && fused_v < 0.20f) fused_v = 0.0f;
        if (fused_v < 0) fused_v = 0;
    }
    if (fused_v < 0.05f) fused_v = 0.0f;
    fused_alt += fused_v * FUSION_DT;
    if (fused_alt < 0) fused_alt = 0;
}


/* ===== 模拟飞行数据 ===== */
static void sim_flight(flight_t *f, float dt)
{
    f->t += dt;
    float thrust = 0, drag = 0.002f * f->v * f->v;
    if (f->t < 1.0f)      { f->phase = 0; thrust = 0; }
    else if (f->t < 2.5f)  { f->phase = 1; thrust = 30; }
    else if (f->t < 12.0f) { f->phase = 2; thrust = 45; }
    else                    { f->phase = 3; thrust = 0; }
    float dv = (thrust - drag - 9.81f) * dt;
    if (f->v + dv < 0) dv = -f->v;
    f->v += dv;
    f->alt += f->v * dt;
    if (f->alt < 0) f->alt = 0;
    f->az = 1.0f + (thrust - drag) / 9.81f;
    f->ax = 0.03f * sinf(f->t * 0.7f);
    f->ay = 0.02f * cosf(f->t * 0.5f);
    f->pitch = 25.0f * sinf(f->t * 0.15f) * (f->phase >= 1 ? 1 : 0);
    f->roll = 15.0f * sinf(f->t * 0.4f) * (f->phase >= 1 ? 1 : 0);
    f->yaw = 8.0f * cosf(f->t * 0.1f) * (f->phase >= 1 ? 1 : 0);
    f->sat = 4 + (int)(2 * sinf(f->t * 0.2f) + 2);
    if (f->sat < 3) f->sat = 3;
    f->lora_ok = (f->t > 2) ? 1 : 0;
    f->pressure = 101325.0f * powf(1.0f - f->alt * 0.0000225577f, 5.25588f);
    f->temperature = 20.0f - f->alt * 0.0065f;
}

#define HIST_N 90
static float v_hist[HIST_N], a_hist[HIST_N], alt_hist[HIST_N];
static float pit_hist[HIST_N], rol_hist[HIST_N], yaw_hist[HIST_N];
static float ax_hist[HIST_N], ay_hist[HIST_N], az_hist[HIST_N];
static int hist_idx = 0;

/* 把数值映射到图表纵坐标 */
static int chart_y(int gy, int gh, float v, float lo, float hi)
{
    int y;
    if (hi - lo < 1e-6f) return gy + gh / 2;
    y = gy + gh - (int)((v - lo) / (hi - lo) * (gh - 1));
    if (y < gy) y = gy;
    if (y > gy + gh - 1) y = gy + gh - 1;
    return y;
}

/* 多曲线图：自适应量程 + 带数值的横向网格 + 图例当前值 */
static void draw_multi_chart(int x0, int y0, int w, int h,
                             float *d1, float *d2, float *d3,
                             uint16_t c1, uint16_t c2, uint16_t c3,
                             const char *title,
                             const char *l1, const char *l2, const char *l3,
                             int start)
{
    static char buf[24];
    float *sets[3];
    uint16_t cols[3];
    const char *labs[3];
    float lo = 1e9f, hi = -1e9f, pad, range;
    int i, k, gx, gy, gw, gh;

    sets[0] = d1; sets[1] = d2; sets[2] = d3;
    cols[0] = c1; cols[1] = c2; cols[2] = c3;
    labs[0] = l1; labs[1] = l2; labs[2] = l3;

    fill_round_rect(x0, y0, w, h, 6, C_DARKER);

    for (k = 0; k < 3; k++)
    {
        if (!sets[k]) continue;
        for (i = 0; i < HIST_N; i++)
        {
            float v = sets[k][i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (hi - lo < 1e-3f) { float c = (hi + lo) * 0.5f; lo = c - 1.0f; hi = c + 1.0f; }
    pad = (hi - lo) * 0.12f;
    lo -= pad; hi += pad;
    range = hi - lo;

    gx = x0 + 36; gy = y0 + 20; gw = w - 44; gh = h - 28;
    if (gw < 10 || gh < 10) return;

    /* 横向网格线 + 对应数值（随曲线量程自适应） */
    for (k = 0; k <= 3; k++)
    {
        int yy = gy + gh * k / 3;
        float v = hi - range * k / 3.0f;
        hline(gx, gx + gw, yy, C_DARK);
        snprintf(buf, sizeof(buf), v >= 100 ? "%.0f" : (v >= 10 ? "%.0f" : "%.1f"), v);
        text(x0 + 2, yy - 3, buf, C_LABEL);
    }
    /* 纵向网格 */
    for (k = 1; k < 4; k++)
        vline(gx + gw * k / 4, gy, gy + gh, C_DARK);

    /* 中文标题 */
    text_cn(x0 + 6, y0 + 1, title, C_LABEL, 1);

    /* 三条曲线：用连线画成实线并加粗到 2px，保证肉眼清楚看到三条 */
    for (k = 0; k < 3; k++)
    {
        int cx;
        if (!sets[k]) continue;
        for (cx = 1; cx < HIST_N; cx++)
        {
            int i0 = (start - (cx - 1) + HIST_N * 2) % HIST_N;
            int i1 = (start - cx + HIST_N * 2) % HIST_N;
            int xa = gx + gw - 1 - (cx - 1) * gw / HIST_N;
            int xb = gx + gw - 1 - cx * gw / HIST_N;
            int ya = chart_y(gy, gh, sets[k][i0], lo, hi);
            int yb = chart_y(gy, gh, sets[k][i1], lo, hi);
            line(xa, ya, xb, yb, cols[k]);
            line(xa, ya + 1, xb, yb + 1, cols[k]);   /* 加粗一行 */
        }
    }

    /* 图例：色块 + 中文名称 + 当前值（右上角） */
    {
        int lx = x0 + w - 40;
        /* 从右往左排版，保证不超出图表右边 */
        for (k = 2; k >= 0; k--)
        {
            int idx = (start + HIST_N) % HIST_N;
            int cw2;
            float cur;
            if (!sets[k]) continue;
            cw2 = labs[k] ? cn_width(labs[k], 1) : 0;
            snprintf(buf, sizeof(buf), "%.0f", sets[k][idx]);
            lx -= (cw2 + (int)strlen(buf) * 8 + 22);
            fill_rect(lx, y0 + 6, 10, 10, cols[k]);
            if (labs[k]) text_cn(lx + 13, y0 + 2, labs[k], cols[k], 1);
            text(lx + 13 + cw2, y0 + 6, buf, cols[k]);
        }
    }
}

static void draw_chart(int x0, int y0, int w, int h, float *data,
                       float maxv, uint16_t color, const char *label,
                       const char *valstr, int start)
{
    int i, cx;
    fill_rect(x0, y0, w, h, C_DARKER);
    for (i = 1; i < 4; i++) {
        hline(x0, x0 + w - 1, y0 + h * i / 4, C_DARK);
        vline(x0 + w * i / 6, y0, y0 + h - 1, C_DARK);
    }
    for (cx = 0; cx < HIST_N && cx < w - 2; cx++) {
        int idx = (start - cx + HIST_N * 2) % HIST_N;
        float val = data[idx];
        int py = y0 + h / 2 - (int)(val / maxv * (h / 2 - 2));
        if (py < y0 + 1) py = y0 + 1;
        if (py > y0 + h - 1) py = y0 + h - 1;
        px(x0 + w - 2 - cx, py, color);
    }
    text_cn(x0 + 4, y0 + 2, label, C_YELLOW, 1);
    if (valstr[0]) text_scale(x0 + w - 80, y0 + h - 16, valstr, color, 2);
}

/* ===== 页面标题 + 大页码 ===== */
static void draw_page_header(const char *title, int page)
{
    /* 标题栏 - 中文标题 */
    fill_rect(SAFE_X, SAFE_Y, SAFE_W, 28, 0x0220);
    fill_round_rect(SAFE_X, SAFE_Y, SAFE_W, 28, 6, 0x0220);
    text_cn(SAFE_X + 6, SAFE_Y + 6, title, C_WHITE, 1);
    if (telemetry_live)
        text_cn(SAFE_X + SAFE_W - 100, SAFE_Y + 6, "已连接", C_LIME, 1);

    /* 页码 - 大字 1/4 */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, NUM_PAGES);
    text_scale(SAFE_X + SAFE_W - 48, SAFE_Y + 5, buf, C_CYAN, 2);
}

/* ===== Page 0: 卫星/连接 ===== */
static void draw_page_sat(flight_t *f)
{
    char buf[64];
    int y = SAFE_Y + 54;
    draw_page_header("状态", 0);
    if (f->time_str[0] && strcmp(f->time_str, "NO_TIME") != 0)
        text_scale(SAFE_X + 8, SAFE_Y + 31, f->time_str, C_WHITE, 2);

    /* 卫星数 - 超大字 */
    fill_round_rect(SAFE_X, y, SAFE_W, 80, 8, C_DARKER);
    text_cn(SAFE_X + 12, y + 10, "卫星", C_YELLOW, 1);
    snprintf(buf, sizeof(buf), "%d", f->sat);
    text_scale(SAFE_X + 80, y + 4, buf, C_GREEN, 4);
    /* 信号条 */
    int i;
    for (i = 0; i < 8; i++)
    {
        int bh = 12 + i * 6;
        fill_rect(SAFE_X + 220 + i * 14, y + 68 - bh, 10, bh,
                  (i < f->sat - 2) ? C_GREEN : C_GRAY);
    }
    y += 88;

    /* GPS */
    fill_round_rect(SAFE_X, y, SAFE_W / 2 - 4, 50, 6, C_DARKER);
    fill_rect(SAFE_X, y, 4, 50, C_SKYBLUE);
    text_cn(SAFE_X + 10, y + 6, "定位", C_SKYBLUE, 1);
    text_cn(SAFE_X + 10, y + 24, f->gps_fix ? "正常" : "搜索中",
            f->gps_fix ? C_GREEN : C_YELLOW, 1);

    /* LoRa：显示本端接收状态即可。
     * 曾经加过"箭载是否收得到本端"的双向显示，实测那条判据不稳定
     * （箭载端边发遥测边收，命中率低、还会误报），已撤掉。
     * 本端收不到箭载遥测是最直接可靠的判据。 */
    fill_round_rect(SAFE_X + SAFE_W / 2 + 4, y, SAFE_W / 2 - 4, 50, 6, C_DARKER);
    fill_rect(SAFE_X + SAFE_W / 2 + 4, y, 4, 50, C_ORANGE);
    text_cn(SAFE_X + SAFE_W / 2 + 14, y + 6, "连接", C_ORANGE, 1);
    fill_circle(SAFE_X + SAFE_W / 2 + 14, y + 32, 5, f->lora_ok ? C_GREEN : C_RED);
    text_cn(SAFE_X + SAFE_W / 2 + 26, y + 24, f->lora_ok ? "正常" : "断开",
            f->lora_ok ? C_GREEN : C_RED, 1);
    y += 58;

    /* 飞行阶段：中文显示 */
    fill_round_rect(SAFE_X, y, SAFE_W, 74, 6, C_DARKER);
    fill_rect(SAFE_X, y, 4, 74, C_MAGENTA);
    text_cn(SAFE_X + 10, y + 4, "阶段", C_LABEL, 1);
    const char *phases_cn[] = {"待命", "确认", "点火", "飞行", "结束", "灭火"};
    uint16_t pc[] = {C_YELLOW, C_ORANGE, C_RED, C_GREEN, C_CYAN, C_MAGENTA};
    int ph = f->phase;
    if (ph < 0 || ph > 5) ph = 0;
    text_cn(SAFE_X + 10, y + 22, phases_cn[ph], pc[ph], 2);
    snprintf(buf, sizeof(buf), "T+%.0fs", f->t);
    text_scale(SAFE_X + SAFE_W - 80, y + 24, buf, C_WHITE, 2);
    /* ESP32 在线状态：由 A:xx 命令回执或每 2 秒的 P? 探测刷新，3 秒无信号即过期。
     * 字库只有 86 字（见 gen_cn_font.py 的 CHARS），"回/应/等/待"都不在字库里，
     * 原来写"已回应/等待回应"会渲染成残缺的"已"，这里改用字库内的字。 */
    text_cn(SAFE_X + 10, y + 56, esp_ack_ms ? "已连接" : "已断开",
            esp_ack_ms ? C_LIME : C_GRAY, 1);
    if (esp_ack_ms)
    {
        snprintf(buf, sizeof(buf), "%s", esp_ack);
        text(SAFE_X + 74, y + 58, buf, C_LIME);
    }
    y += 82;

    /* 温度 + 气压 并排 */
    fill_round_rect(SAFE_X, y, SAFE_W / 2 - 4, 40, 6, C_DARKER);
    fill_rect(SAFE_X, y, 4, 40, C_ORANGE);
    text_cn(SAFE_X + 10, y + 4, "温度", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.1fC", f->temperature);
    text_scale(SAFE_X + 10, y + 20, buf, C_ORANGE, 2);

    fill_round_rect(SAFE_X + SAFE_W / 2 + 4, y, SAFE_W / 2 - 4, 40, 6, C_DARKER);
    fill_rect(SAFE_X + SAFE_W / 2 + 4, y, 4, 40, C_CYAN);
    text_cn(SAFE_X + SAFE_W / 2 + 14, y + 4, "气压", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.1fkPa", f->pressure / 1000.0f);
    text_scale(SAFE_X + SAFE_W / 2 + 14, y + 20, buf, C_CYAN, 2);
    y += 48;

    /* 坐标：地基（思澈卫星）与火箭（ESP32），第一页底部实时显示 */
    fill_round_rect(SAFE_X, y, SAFE_W, 58, 6, C_DARKER);
    fill_rect(SAFE_X, y, 4, 58, C_SKYBLUE);
    text_cn(SAFE_X + 10, y + 4, "坐标", C_LABEL, 1);
    if (own_fix)
        snprintf(buf, sizeof(buf), "%.5f %.5f", own_lat, own_lon);
    else
        snprintf(buf, sizeof(buf), "-- 等待定位 --");
    text(SAFE_X + 74, y + 6, buf, C_CYAN);
    text_cn(SAFE_X + 10, y + 24, "火箭", C_LABEL, 1);
    if (f->gps_fix)
        snprintf(buf, sizeof(buf), "%.5f %.5f", f->lat, f->lon);
    else
        snprintf(buf, sizeof(buf), "-- 无定位 --");
    text(SAFE_X + 74, y + 26, buf, C_YELLOW);
}

/* ===== Page 1: 速度/加速度 ===== */
static void draw_page_vel(flight_t *f)
{
    char buf[64];
    int y = SAFE_Y + 36;
    draw_page_header("速度", 2);

    /* 速度大字 */
    fill_round_rect(SAFE_X, y, SAFE_W, 70, 8, C_DARKER);
    fill_rect(SAFE_X, y, 4, 70, C_GREEN);
    text_cn(SAFE_X + 12, y + 6, "速度", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.1f", f->v);
    text_scale(SAFE_X + 12, y + 26, buf, C_GREEN, 3);
    text_scale(SAFE_X + 210, y + 40, "m/s", C_LABEL, 1);
    y += 78;

    /* 加速度三栏（含重力原始值） */
    fill_round_rect(SAFE_X, y, SAFE_W / 3 - 4, 62, 6, C_DARKER);
    fill_rect(SAFE_X, y, 4, 62, C_RED);
    text_cn(SAFE_X + 10, y + 6, "X轴", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%+.2f", f->ax);
    text_scale(SAFE_X + 10, y + 28, buf, C_RED, 2);

    fill_round_rect(SAFE_X + SAFE_W / 3 + 2, y, SAFE_W / 3 - 4, 62, 6, C_DARKER);
    fill_rect(SAFE_X + SAFE_W / 3 + 2, y, 4, 62, C_GREEN);
    text_cn(SAFE_X + SAFE_W / 3 + 12, y + 6, "Y轴", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%+.2f", f->ay);
    text_scale(SAFE_X + SAFE_W / 3 + 12, y + 28, buf, C_GREEN, 2);

    fill_round_rect(SAFE_X + 2 * SAFE_W / 3 + 4, y, SAFE_W / 3 - 4, 62, 6, C_DARKER);
    fill_rect(SAFE_X + 2 * SAFE_W / 3 + 4, y, 4, 62, C_BLUE);
    text_cn(SAFE_X + 2 * SAFE_W / 3 + 14, y + 6, "Z轴", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.2f", f->az);
    text_scale(SAFE_X + 2 * SAFE_W / 3 + 14, y + 28, buf, C_BLUE, 2);
    y += 70;

    /* 速度曲线（拉高填满底部，不留黑空） */
    snprintf(buf, sizeof(buf), "%.0f", f->v);
    draw_chart(SAFE_X, y, SAFE_W, 100, v_hist, 150.0f, C_GREEN, "速度曲线", buf, hist_idx);
    y += 108;

    /* 加速度曲线 */
    snprintf(buf, sizeof(buf), "%.1fg", f->az - 1.0f);
    draw_chart(SAFE_X, y, SAFE_W, 100, a_hist, 5.0f, C_RED, "加速度曲线", buf, hist_idx);
}

/* ===== Page 2: 姿态/高度 ===== */
static void draw_attitude(int cx, int cy, int r, float pitch, float roll)
{
    int i;
    int horizon = cy - (int)(pitch * 2.0f);
    float ang = roll * 3.14159f / 180.0f;
    float cs = cosf(ang), sn = sinf(ang);
    for (i = -r; i <= r; i++)
    {
        int wx = cx + i;
        int hx = horizon + (int)(i * sn);
        if (wx >= cx - r && wx <= cx + r)
        {
            int top = cy - (int)sqrtf(r * r - i * i);
            int bot = cy + (int)sqrtf(r * r - i * i);
            int yy;
            for (yy = top; yy <= bot; yy++)
                px(wx, yy, (yy < hx) ? 0x0170 : 0x5A40);
        }
    }
    for (i = -r; i <= r; i++)
    {
        int wx = cx + i;
        int wy = horizon + (int)(i * sn);
        px(wx, wy, C_CYAN);
    }
    draw_circle(cx, cy, r, C_WHITE);
}

static void draw_page_att(flight_t *f)
{
    char buf[64];
    int y = SAFE_Y + 36;
    draw_page_header("姿态", 3);

    /* 姿态仪 */
    draw_attitude(SAFE_X + 80, y + 78, 68, f->pitch, f->roll);
    /* 姿态数值（中文标签） */
    text_cn(SAFE_X + 6, y + 158, "俯", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%+.0f", f->pitch);
    text(SAFE_X + 24, y + 162, buf, C_CYAN);
    text_cn(SAFE_X + 84, y + 158, "滚", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%+.0f", f->roll);
    text(SAFE_X + 102, y + 162, buf, C_MAGENTA);
    text_cn(SAFE_X + 162, y + 158, "偏", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%+.0f", f->yaw);
    text(SAFE_X + 180, y + 162, buf, C_YELLOW);

    /* 右侧高度 */
    fill_round_rect(SAFE_X + 200, y, SAFE_W - 200, 70, 8, C_DARKER);
    fill_rect(SAFE_X + 200, y, 4, 70, C_GREEN);
    text_cn(SAFE_X + 210, y + 6, "高度", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.0f", f->alt);
    text_scale(SAFE_X + 210, y + 26, buf, C_GREEN, 3);

    /* 右侧气压 */
    fill_round_rect(SAFE_X + 200, y + 78, SAFE_W - 200, 60, 8, C_DARKER);
    fill_rect(SAFE_X + 200, y + 78, 4, 60, C_CYAN);
    text_cn(SAFE_X + 210, y + 84, "气压", C_LABEL, 1);
    snprintf(buf, sizeof(buf), "%.1f", f->pressure / 1000.0f);
    text_scale(SAFE_X + 210, y + 102, buf, C_CYAN, 2);

    /* 高度曲线 + 速度曲线：填满底部，不留黑空 */
    y = SAFE_Y + 36 + 176;
    snprintf(buf, sizeof(buf), "%.0f", f->alt);
    draw_chart(SAFE_X, y, SAFE_W, 92, alt_hist, 500.0f, C_BLUE, "高度曲线", buf, hist_idx);
    y += 100;
    snprintf(buf, sizeof(buf), "%.0f", f->v);
    draw_chart(SAFE_X, y, SAFE_W, 92, v_hist, 150.0f, C_GREEN, "速度曲线", buf, hist_idx);
}

/* ===== Page 3: 火箭倾斜 (大幅重画) ===== */
static void draw_page_rocket(flight_t *f)
{
    char buf[64];
    int cx = fb_w / 2;
    int cy = fb_h / 2;  /* 屏幕正中 */
    float pitch_rad = f->pitch * 3.14159f / 180.0f;
    float roll_rad = f->roll * 3.14159f / 180.0f;
    float yaw_rad = f->yaw * 3.14159f / 180.0f;
    int i;

    draw_page_header("火箭", 4);
    fill_rect(SAFE_X, SAFE_Y + 36, SAFE_W, SAFE_H - 60, C_BLACK);

    /* ===== XYZ 轴 (贯穿全屏, 粗线) ===== */
    int ax_len = 180;
    int x_end_x = cx + (int)(ax_len * cosf(roll_rad));
    int x_end_y = cy + (int)(ax_len * sinf(roll_rad));
    int y_end_x = cx + (int)(ax_len * sinf(pitch_rad) * 0.5f);
    int y_end_y = cy - (int)(ax_len * cosf(pitch_rad) * 0.5f);
    int z_end_x = cx + (int)(ax_len * sinf(yaw_rad) * 0.7f);
    int z_end_y = cy + (int)(ax_len * cosf(yaw_rad) * 0.3f);

    thick_line(cx, cy, x_end_x, x_end_y, C_RED, 4);
    thick_line(cx, cy, y_end_x, y_end_y, C_GREEN, 4);
    thick_line(cx, cy, z_end_x, z_end_y, C_BLUE, 4);

    text_scale(x_end_x + 4, x_end_y - 8, "X", C_RED, 2);
    text_scale(y_end_x + 4, y_end_y - 16, "Y", C_GREEN, 2);
    text_scale(z_end_x + 4, z_end_y - 8, "Z", C_BLUE, 2);

    /* ===== 火箭 (大幅加宽加高) ===== */
    int body_h = 200;  /* 箭体高度 */
    int body_w = 160;  /* 箭体宽度 (大幅加宽!) */
    int nose_h = 60;   /* 箭头高度 */
    int fin_h = 35;    /* 尾翼高度 */
    int fin_w = 50;    /* 尾翼宽度 */

    /* 倾斜偏移 */
    int tilt_x = (int)(sinf(roll_rad) * body_h * 0.4f);
    int tilt_y = (int)(sinf(pitch_rad) * body_h * 0.3f);

    /* 箭体位置 */
    int body_top_y = cy - body_h / 2 + tilt_y / 2;
    int body_bot_y = cy + body_h / 2 + tilt_y / 2;
    int body_left_x = cx - body_w / 2 + tilt_x / 3;
    int body_right_x = cx + body_w / 2 + tilt_x / 3;

    /* 箭体主体 (橙色填充) */
    for (i = 0; i < body_w; i++)
    {
        int lx = body_left_x + i;
        int tx = lx + tilt_x / 2;
        line(lx, body_top_y, tx, body_bot_y, C_ORANGE);
    }

    /* 箭体中间条纹 (白色) */
    int stripe_y = cy + tilt_y / 2;
    fill_rect(body_left_x + 5, stripe_y - 3, body_w - 10, 6, C_WHITE);

    /* 箭头 (红色三角) */
    for (i = 0; i < nose_h; i++)
    {
        int w = (nose_h - i) * body_w / (2 * nose_h);
        int nx = cx + tilt_x / 2 + (int)(tilt_x * i / body_h);
        int ny = body_top_y - i;
        hline(nx - w, nx + w, ny, C_RED);
    }

    /* 尾翼 (橙色) */
    for (i = 0; i < fin_h; i++)
    {
        int w = fin_w - i * fin_w / fin_h;
        hline(body_left_x - w, body_left_x, body_bot_y + i, C_ORANGE);
        hline(body_right_x, body_right_x + w, body_bot_y + i, C_ORANGE);
    }

    /* 火焰 */
    if (f->phase >= 1 && f->phase <= 2)
    {
        int flame_h = 40 + (int)(15 * sinf(f->t * 20.0f));
        for (i = 0; i < flame_h; i++)
        {
            int fw = (flame_h - i) * 5 / flame_h + 1;
            uint16_t fc = (i < 10) ? C_YELLOW : ((i < 25) ? C_ORANGE : C_RED);
            int fx = cx + tilt_x / 2;
            hline(fx - fw, fx + fw, body_bot_y + fin_h + 4 + i, fc);
        }
    }

    /* 中心原点 */
    fill_circle(cx, cy, 5, C_WHITE);

    /* 倾角指示 (右上角大环) */
    draw_circle(340, 55, 30, C_GRAY);
    int dot_x = 340 + (int)(sinf(roll_rad) * 24);
    int dot_y = 55 - (int)(sinf(pitch_rad) * 24);
    fill_circle(dot_x, dot_y, 6, C_YELLOW);
    text_cn(316, 90, "倾斜", C_LABEL, 1);

    /* 底部数据 */
    fill_round_rect(SAFE_X, fb_h - SAFE_Y - 50, SAFE_W, 44, 6, C_DARKER);
    snprintf(buf, sizeof(buf), "P:%+.0f R:%+.0f Y:%+.0f", f->pitch, f->roll, f->yaw);
    text(SAFE_X + 10, fb_h - SAFE_Y - 42, buf, C_YELLOW);
    snprintf(buf, sizeof(buf), "V:%.0f  ALT:%.0f  T+%.0f", f->v, f->alt, f->t);
    text(SAFE_X + 10, fb_h - SAFE_Y - 26, buf, C_GREEN);
}

/* ===== Page 3 replacement: large 3D-style rocket ===== */
/* 3D 刚体旋转: 火箭纵轴沿 +Y, 横轴 +X, 深度 +Z */
typedef struct { float x, y, z; } vec3;

static void rot3(vec3 *v, float yaw, float pitch, float roll)
{
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll), sr = sinf(roll);
    float x = v->x, y = v->y, z = v->z;
    float x1 = cr * x + sr * z;          /* 绕 Y (roll) */
    float z1 = -sr * x + cr * z;
    float y2 = cp * y - sp * z1;         /* 绕 X (pitch) */
    float z2 = sp * y + cp * z1;
    v->x = cy * x1 - sy * y2;            /* 绕 Z (yaw) */
    v->y = sy * x1 + cy * y2;
    v->z = z2;
}

/* 等距正交投影: 单位向量 → 屏幕偏移 (三轴呈 120°) */
static void proj_dir(float x, float y, float z, float *dx, float *dy)
{
    *dx = (x - z) * 0.866f;
    *dy = -y + (x + z) * 0.5f;
}

/* 将机体坐标点投影到屏幕，保证火箭圆截面也跟随同一刚体旋转 */
static void rocket_point(int cx, int cy, float t, float rx, float rz,
                         float dxx, float dxy, float dyx, float dyy,
                         float dzx, float dzy, int *sx, int *sy)
{
    *sx = (int)(cx + t * dyx + rx * dxx + rz * dzx);
    *sy = (int)(cy + t * dyy + rx * dxy + rz * dzy);
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                     uint16_t color)
{
    int y, lo, hi, tmp;
    int ymin = y0, ymax = y0;
    if (y1 < ymin) ymin = y1; if (y2 < ymin) ymin = y2;
    if (y1 > ymax) ymax = y1; if (y2 > ymax) ymax = y2;
    if (ymin < 0) ymin = 0; if (ymax >= fb_h) ymax = fb_h - 1;
    for (y = ymin; y <= ymax; y++)
    {
        float e[3];
        int n = 0;
        if (y1 != y0 && ((y0 <= y && y < y1) || (y1 <= y && y < y0)))
            e[n++] = x0 + (float)(y - y0) * (x1 - x0) / (y1 - y0);
        if (y2 != y1 && ((y1 <= y && y < y2) || (y2 <= y && y < y1)))
            e[n++] = x1 + (float)(y - y1) * (x2 - x1) / (y2 - y1);
        if (y0 != y2 && ((y2 <= y && y < y0) || (y0 <= y && y < y2)))
            e[n++] = x2 + (float)(y - y2) * (x0 - x2) / (y0 - y2);
        if (n < 2) continue;
        lo = (int)e[0]; hi = (int)e[1];
        if (lo > hi) { tmp = lo; lo = hi; hi = tmp; }
        if (lo < 0) lo = 0; if (hi >= fb_w) hi = fb_w - 1;
        hline(lo, hi, y, color);
    }
}

static void project_local(int cx, int cy, float lx, float ly, float lz,
                          float yaw, float pitch, float roll,
                          int *sx, int *sy, float *depth)
{
    vec3 p = {lx, ly, lz};
    float den;
    rot3(&p, yaw, pitch, roll);
    den = 260.0f - p.z;
    if (den < 80.0f) den = 80.0f;
    *depth = den;
    *sx = cx + (int)(p.x * 1.45f * 260.0f / den);
    *sy = cy - (int)(p.y * 1.45f * 260.0f / den);
}

static void draw_solid_rocket(int cx, int cy, float yaw, float pitch, float roll)
{
    static const uint16_t shade[8] = {
        0x4208, 0x630C, 0x8410, 0xA514, 0xC618, 0xE71C, 0xF800, 0x7BEF
    };
    int bx[8], by[8], fx[8], fy[8], i, j, k;
    float bd, fd, a, r = 23.0f;
    int order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    /* 远端环先画，近端环后画，形成实体遮挡 */
    for (i = 0; i < 8; i++)
    {
        a = 6.2831853f * i / 8.0f + 0.3926991f;
        project_local(cx, cy, cosf(a) * r, -65.0f, sinf(a) * r,
                      yaw, pitch, roll, &bx[i], &by[i], &bd);
        project_local(cx, cy, cosf(a) * r, 65.0f, sinf(a) * r,
                      yaw, pitch, roll, &fx[i], &fy[i], &fd);
    }
    for (i = 0; i < 8; i++)
        for (j = i + 1; j < 8; j++)
            if (bd < fd) { k = order[i]; order[i] = order[j]; order[j] = k; }
    for (i = 0; i < 8; i++)
    {
        j = order[i];
        k = (j + 1) & 7;
        fill_tri(bx[j], by[j], bx[k], by[k], fx[k], fy[k], shade[j]);
        fill_tri(bx[j], by[j], fx[k], fy[k], fx[j], fy[j], shade[j]);
    }
    /* 可见端面：八边形三角扇，明确显示圆柱口 */
    for (i = 0; i < 8; i++)
    {
        j = (i + 1) & 7;
        fill_tri(fx[0], fy[0], fx[i], fy[i], fx[j], fy[j],
                 i & 1 ? 0xC618 : 0xE71C);
    }
}

static void draw_ring(int cx, int cy, float t, float radius,
                      float dxx, float dxy, float dyx, float dyy,
                      float dzx, float dzy, uint16_t color)
{
    int i, px0 = 0, py0 = 0;
    for (i = 0; i <= 32; i++)
    {
        float a = 6.2831853f * i / 32.0f;
        int x, y;
        rocket_point(cx, cy, t, cosf(a) * radius, sinf(a) * radius,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x, &y);
        if (i > 0) line(px0, py0, x, y, color);
        px0 = x;
        py0 = y;
    }
}

static void draw_page_rocket_big(flight_t *f)
{
    char buf[64];
    const int cx = fb_w / 2;
    const int cy = 205;
    const int axis_len = 130;
    const float yaw = f->yaw * 3.14159f / 180.0f;
    const float pitch = f->pitch * 3.14159f / 180.0f;
    const float roll = f->roll * 3.14159f / 180.0f;
    int i;

    draw_page_header("火箭", 4);
    fill_rect(SAFE_X, SAFE_Y + 36, SAFE_W, SAFE_H - 60, C_BLACK);

    /* 机体单位轴 → 姿态旋转 → 投影成屏幕方向向量 */
    vec3 ux = {1, 0, 0}, uy = {0, 1, 0}, uz = {0, 0, 1};
    rot3(&ux, yaw, pitch, roll);
    rot3(&uy, yaw, pitch, roll);
    rot3(&uz, yaw, pitch, roll);
    float dxx, dxy, dyx, dyy, dzx, dzy;
    proj_dir(ux.x, ux.y, ux.z, &dxx, &dxy);
    proj_dir(uy.x, uy.y, uy.z, &dyx, &dyy);
    proj_dir(uz.x, uz.y, uz.z, &dzx, &dzy);

    /* XYZ 轴 (刚体轴, 恒互相垂直) */
    int xx = cx + (int)(dxx * axis_len);
    int xy = cy + (int)(dxy * axis_len);
    int yx = cx + (int)(dyx * axis_len);
    int yy = cy + (int)(dyy * axis_len);
    int zx = cx + (int)(dzx * axis_len);
    int zy = cy + (int)(dzy * axis_len);
    thick_line(cx, cy, xx, xy, C_RED, 4);
    thick_line(cx, cy, yx, yy, C_GREEN, 4);
    thick_line(cx, cy, zx, zy, C_BLUE, 4);
    text_scale(xx + (dxx >= 0 ? 5 : -18), xy + (dxy >= 0 ? 5 : -18), "X", C_RED, 2);
    text_scale(yx + (dyx >= 0 ? 5 : -18), yy + (dyy >= 0 ? 5 : -18), "Y", C_GREEN, 2);
    text_scale(zx + (dzx >= 0 ? 5 : -18), zy + (dzy >= 0 ? 5 : -18), "Z", C_BLUE, 2);

    /* 低开销圆柱立体火箭：三条明暗带模拟圆截面，整体绑定机体坐标 */
    const float body_h = 130.0f;
    const float body_r = 21.0f;
    const float nose_h = 34.0f;
    const float fin_h = 25.0f;
    const float fin_w = 23.0f;

    /* 原始火箭本体和 XYZ 坐标保留；不叠加额外固定壳体 */
    /* 前后端面和纵向轮廓，明确显示圆柱的深度 */
    draw_ring(cx, cy, -body_h / 2.0f, body_r, dxx, dxy, dyx, dyy, dzx, dzy, C_LGRAY);
    draw_ring(cx, cy, body_h / 2.0f, body_r, dxx, dxy, dyx, dyy, dzx, dzy, C_WHITE);
    draw_ring(cx, cy, body_h / 2.0f + nose_h, 1.0f, dxx, dxy, dyx, dyy, dzx, dzy, C_RED);
    {
        float angles[4] = {0.0f, 1.5708f, 3.1416f, 4.7124f};
        for (i = 0; i < 4; i++)
        {
            int x0, y0, x1, y1;
            float a = angles[i];
            rocket_point(cx, cy, -body_h / 2.0f, cosf(a) * body_r, sinf(a) * body_r,
                         dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
            rocket_point(cx, cy, body_h / 2.0f, cosf(a) * body_r, sinf(a) * body_r,
                         dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
            line(x0, y0, x1, y1, i == 0 ? C_WHITE : C_GRAY);
        }
    }

    /* 外轮廓、暗侧、高光：每层仅三条线，避免阻塞触摸 */
    for (i = 0; i <= (int)body_h; i += 4)
    {
        float t = -body_h / 2.0f + i;
        int x0, y0, x1, y1;
        rocket_point(cx, cy, t, -body_r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, body_r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_GRAY);
        rocket_point(cx, cy, t, -body_r * 0.72f, body_r * 0.35f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, body_r * 0.72f, body_r * 0.35f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_LGRAY);
        rocket_point(cx, cy, t, -body_r * 0.45f, body_r * 0.62f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, body_r * 0.45f, body_r * 0.62f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_WHITE);

        /* 两道装饰带直接覆盖在圆柱表面 */
        if (i > body_h * 0.25f && i < body_h * 0.31f)
            line(x0, y0, x1, y1, C_RED);
        if (i > body_h * 0.68f && i < body_h * 0.74f)
            line(x0, y0, x1, y1, C_BLUE);
    }

    /* 头部圆锥：外轮廓 + 高光线 */
    for (i = 0; i <= (int)nose_h; i += 3)
    {
        float t = body_h / 2.0f + i;
        float r = body_r * (nose_h - i) / nose_h;
        int x0, y0, x1, y1;
        rocket_point(cx, cy, t, -r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_DARKRED);
        rocket_point(cx, cy, t, -r * 0.45f, r * 0.65f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, r * 0.45f, r * 0.65f,
                     dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_RED);
    }

    /* 两片尾翼 */
    for (i = 0; i <= (int)fin_h; i += 3)
    {
        float t = -body_h / 2.0f - i;
        float w = fin_w * i / fin_h;
        int x0, y0, x1, y1;
        rocket_point(cx, cy, t, -body_r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, -(body_r + w), 0, dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_RED);
        rocket_point(cx, cy, t, body_r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
        rocket_point(cx, cy, t, body_r + w, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
        line(x0, y0, x1, y1, C_RED);
    }

    /* 火焰 */
    if (f->phase >= 1 && f->phase <= 2)
    {
        int flame_h = 30 + (int)(7.0f * sinf(f->t * 20.0f));
        for (i = 0; i < flame_h; i += 2)
        {
            float t = -body_h / 2.0f - fin_h - i;
            float r = (flame_h - i) * 5.0f / flame_h + 1.0f;
            int x0, y0, x1, y1;
            rocket_point(cx, cy, t, -r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x0, &y0);
            rocket_point(cx, cy, t, r, 0, dxx, dxy, dyx, dyy, dzx, dzy, &x1, &y1);
            line(x0, y0, x1, y1, i < 8 ? C_YELLOW : (i < 20 ? C_ORANGE : C_RED));
        }
    }

    /* Tilt indicator and values */
    draw_circle(fb_w - SAFE_X - 30, SAFE_Y + 55, 26, C_GRAY);
    fill_circle(fb_w - SAFE_X - 30 + (int)(sinf(roll) * 20),
                SAFE_Y + 55 - (int)(sinf(pitch) * 20), 5, C_YELLOW);
    text_cn(fb_w - SAFE_X - 60, SAFE_Y + 88, "倾斜", C_LABEL, 1);

    fill_round_rect(SAFE_X, fb_h - SAFE_Y - 50, SAFE_W, 44, 6, C_DARKER);
    snprintf(buf, sizeof(buf), "P:%+.0f R:%+.0f Y:%+.0f", f->pitch, f->roll, f->yaw);
    text(SAFE_X + 10, fb_h - SAFE_Y - 42, buf, C_YELLOW);
    snprintf(buf, sizeof(buf), "V:%.0f ALT:%.0f T+%.0f", f->v, f->alt, f->t);
    text(SAFE_X + 10, fb_h - SAFE_Y - 26, buf, C_GREEN);
}

/* ===== Page 4: 三姿态 / 三加速度 / 高度 曲线 =====
 * 布局（390x450，安全区 18..432）：
 *   header 18..46
 *   姿态图 52..164   加速度图 172..284   高度图 292..392
 * 三张图都在安全区内，不会挤出屏幕。
 */
static void draw_page_curves(flight_t *f)
{
    int y = SAFE_Y + 34;
    draw_page_header("曲线", 1);

    draw_multi_chart(SAFE_X, y, SAFE_W, 112,
                     pit_hist, rol_hist, yaw_hist,
                     C_CYAN, C_MAGENTA, C_YELLOW,
                     "姿态曲线",
                     "俯", "滚", "偏", hist_idx);
    y += 120;

    draw_multi_chart(SAFE_X, y, SAFE_W, 112,
                     ax_hist, ay_hist, az_hist,
                     C_RED, C_GREEN, C_BLUE,
                     "加速度曲线",
                     "X", "Y", "Z", hist_idx);
    y += 120;

    draw_multi_chart(SAFE_X, y, SAFE_W, 100,
                     alt_hist, NULL, NULL,
                     C_GREEN, C_GREEN, C_GREEN,
                     "高度曲线",
                     "高度", NULL, NULL, hist_idx);
}

/* ===== Page 5: 雷达页 =====
 * 以思澈（地基站）为中心，正北为上，多个带距离刻度的同心圆盘，
 * 圆盘上显示火箭（ESP32）位置；右侧简单列出坐标/高度/地面投影距离。
 * 所有数值都是思澈本地实时计算，不经 LoRa 回传。
 */
static void draw_page_radar(flight_t *f)
{
    char buf[40];
    const int cx = SAFE_X + 118;
    const int cy = SAFE_Y + 150;
    const int rmax = 108;
    float range;
    int i, k;
    float scale;

    draw_page_header("雷达", 5);

    /* 量程自适应：至少 50m，按火箭距离向上取整到 50m 档 */
    range = 50.0f;
    if (rel_valid && rel_ground_m > 1.0f)
    {
        range = 50.0f;
        while (range < rel_ground_m * 1.15f && range < 5000.0f) range += 50.0f;
    }
    scale = (float)rmax / range;

    /* 同心圆盘（4 圈，带距离刻度） */
    for (k = 1; k <= 4; k++)
    {
        int rr = rmax * k / 4;
        draw_circle(cx, cy, rr, (k == 4) ? C_GRAY : C_DARK);
        snprintf(buf, sizeof(buf), "%.0f", range * k / 4.0f);
        text(cx + rr - 6, cy - 6, buf, C_LABEL);
    }
    /* 十字准线 + 正北 */
    vline(cx, cy - rmax, cy + rmax, C_DARK);
    hline(cx - rmax, cx + rmax, cy, C_DARK);
    text_cn(cx - 8, cy - rmax - 18, "北", C_RED, 1);
    text(cx - rmax - 12, cy - 6, "W", C_GRAY);
    text(cx + rmax + 4, cy - 6, "E", C_GRAY);

    /* 地基站（中心） */
    fill_circle(cx, cy, 4, C_GREEN);

    /* 火箭目标点 */
    if (rel_valid)
    {
        float br = d2r(rel_bearing);
        int px_ = cx + (int)(rel_ground_m * scale * sinf(br));
        int py_ = cy - (int)(rel_ground_m * scale * cosf(br));
        if (px_ < SAFE_X) px_ = SAFE_X;
        if (px_ > SAFE_X + rmax * 2) px_ = SAFE_X + rmax * 2;
        line(cx, cy, px_, py_, C_DARK);
        fill_circle(px_, py_, 6, C_YELLOW);
        draw_circle(px_, py_, 10, C_ORANGE);
        snprintf(buf, sizeof(buf), "%.0fm", rel_ground_m);
        text(px_ + 12, py_ - 4, buf, C_YELLOW);
    }
    else
    {
        text_cn(cx - 40, cy + rmax + 12, "等待定位", C_GRAY, 1);
    }

    /* 右侧数据：坐标 / 高度 / 地面投影距离 */
    {
        int bx = SAFE_X + 240;
        int by = SAFE_Y + 44;
        fill_round_rect(bx, by, SAFE_W - 222, 196, 6, C_DARKER);

        text_cn(bx + 8, by + 6, "地基", C_LABEL, 1);
        if (own_fix)
            snprintf(buf, sizeof(buf), "%.5f", own_lat);
        else
            snprintf(buf, sizeof(buf), "--");
        text(bx + 8, by + 26, buf, C_CYAN);
        if (own_fix)
            snprintf(buf, sizeof(buf), "%.5f", own_lon);
        else
            snprintf(buf, sizeof(buf), "--");
        text(bx + 8, by + 38, buf, C_CYAN);

        text_cn(bx + 8, by + 56, "火箭", C_LABEL, 1);
        if (f->gps_fix)
            snprintf(buf, sizeof(buf), "%.5f", f->lat);
        else
            snprintf(buf, sizeof(buf), "--");
        text(bx + 8, by + 76, buf, C_YELLOW);
        if (f->gps_fix)
            snprintf(buf, sizeof(buf), "%.5f", f->lon);
        else
            snprintf(buf, sizeof(buf), "--");
        text(bx + 8, by + 88, buf, C_YELLOW);

        text_cn(bx + 8, by + 106, "距离", C_LABEL, 1);
        snprintf(buf, sizeof(buf), "%.0f m", rel_valid ? rel_ground_m : 0.0f);
        text_scale(bx + 8, by + 124, buf, C_GREEN, 1);

        text_cn(bx + 8, by + 142, "高度差", C_LABEL, 1);
        snprintf(buf, sizeof(buf), "%+.0f m", rel_valid ? rel_alt_m : 0.0f);
        text_scale(bx + 8, by + 160, buf, C_ORANGE, 1);

        text_cn(bx + 8, by + 178, "方位", C_LABEL, 1);
        snprintf(buf, sizeof(buf), "%.0f", rel_valid ? rel_bearing : 0.0f);
        text(bx + 48, by + 180, buf, C_MAGENTA);
    }

    /* 圆盘下方：地基基准按钮（可触摸点击，也可自动校准） */
    {
        int by = cy + rmax + 30;
        int bw = SAFE_W;
        fill_round_rect(SAFE_X, by, bw, 46, 6, C_DARKER);
        fill_rect(SAFE_X, by, 4, 46, own_fix ? C_GREEN : C_ORANGE);

        if (own_manual)
            text_cn(SAFE_X + 12, by + 4, "地基已手动设定", C_LIME, 1);
        else if (own_fix)
            text_cn(SAFE_X + 12, by + 4, "地基已自动校准", C_LIME, 1);
        else
            text_cn(SAFE_X + 12, by + 4, "等待地基校准", C_ORANGE, 1);

        text_cn(SAFE_X + 12, by + 24, "点此用火箭坐标校准", C_LABEL, 1);

        /* 记住按钮区域，供触摸命中判断 */
        radar_btn_x = SAFE_X;
        radar_btn_y = by;
        radar_btn_w = bw;
        radar_btn_h = 46;
    }
}

/* ===== 主程序 ===== */
int main(int argc, char *argv[])
{
    struct fb_videoinfo_s vinfo;
    struct fb_planeinfo_s pinfo;
    struct touch_sample_s sample;
    flight_t fl;
    int i, cur_page = 0, frame = 0;
    int swipe_start_x = -1, touch_down = 0, swiped = 0;
    int prev_page = -1;
    unsigned int draw_tick = 0;
    unsigned int page_auto_tick = 0;

    /* 面板初始化与 NSH/rcS 可能并发，自动启动时不要因一次 fb0 竞态退出。 */
    for (i = 0; i < 30; i++)
    {
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0) break;
        printf("WAIT fb0 %d/30\n", i + 1);
        usleep(500000);
    }
    if (fb_fd < 0) { printf("ERR fb0 after retry\n"); return 1; }
    ioctl(fb_fd, FBIOGET_VIDEOINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_PLANEINFO, &pinfo);
    fb_w = vinfo.xres;
    fb_h = vinfo.yres;
    fb_stride = pinfo.stride / 2;
    fb = (uint16_t *)mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FILE, fb_fd, 0);
    if (fb == MAP_FAILED) fb = (uint16_t *)pinfo.fbmem;

    touch_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
    btn_fd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
    lora_fd = lora_open();
    printf("LoRa UART2 %s\n", lora_fd >= 0 ? "ready" : "unavailable");
    gps_fd = gps_open();
    printf("Sat UART3 %s\n", gps_fd >= 0 ? "ready" : "unavailable");
    printf("Buttons %s\n", btn_fd >= 0 ? "ready" : "unavailable");
    printf("FB %d x %d stride=%d\n", fb_w, fb_h, fb_stride);
    memset(&fl, 0, sizeof(fl));
    strcpy(fl.time_str, "NO_TIME");
    fl.rocket_link = -1;          /* 还没收到箭载自报状态 */
    for (i = 0; i < HIST_N; i++)
    {
        v_hist[i] = 0; a_hist[i] = 0; alt_hist[i] = 0;
        pit_hist[i] = 0; rol_hist[i] = 0; yaw_hist[i] = 0;
        ax_hist[i] = 0; ay_hist[i] = 0; az_hist[i] = 1.0f;
    }

    /* 开机链路自检：思澈 → ESP32 → 回执，确认按键命令通道可用 */
    send_sd_cmd("S:PING");
    printf("LINK self-test sent\n");
    while (1)
    {
        /* 无真实遥测时保持断开/无卫星，不使用模拟飞行数据。 */
        if (!telemetry_live)
        {
            fl.gps_fix = 0;
            fl.sat = 0;
            fl.lora_ok = 0;
            fl.v = 0;
            fl.alt = 0;
            fl.pressure = 0;
            fl.temperature = 0;
        }
        lora_poll(&fl);
        gps_poll();          /* 思澈侧卫星模块（UART3），本地解析 */
        if (!own_manual && !own_sat_ok) ground_autoset(&fl);  /* 待命时用箭载坐标当发射点 */
        rel_update(&fl);     /* 本地实时计算相对距离/方位/高度差 */
        cmd_tick();          /* 命令重发队列（非阻塞） */
        handle_buttons(&fl);
        fusion_update(&fl);
        fl.phase = mission_phase;
        /* 只有点火/飞行阶段用融合值；其余阶段保留 ESP32 实时下发的
         * 气压高度与 GPS 速度，保证数据时时刻刻都有显示。 */
        if (mission_phase == PHASE_IGNITE || mission_phase == PHASE_FLIGHT)
        {
            fl.v = fused_v;
            fl.alt = fused_alt;
        }
        fl.t += FUSION_DT;
        lora_silence_ticks++;
        if (esp_ack_ms > 0) esp_ack_ms--;   /* 命令回执新鲜度倒计时，过期即为失联 */

        /* 命令通道心跳：只在没有命令在重发时才发，避免思澈自己一直发射
         * 把接收遥测的窗口也堵掉。
         * 链路存活判定现在由 ESP32 每 2 秒的 P? 探测负责，这里只在
         * 6 秒没听到 ESP32 时才补发一次 PING 做兜底，因此比原来更省带宽。 */
        link_tick++;
        if (cmd_repeat <= 0)
        {
            unsigned int limit = 6000;   /* 6 秒兜底一次 */
            if (link_tick >= limit)
            {
                link_tick = 0;
                send_sd_cmd("S:PING");
            }
        }

        /* 约每 150ms 输出一次网页 JSON（含思澈端融合结果与任务阶段） */
        if (telemetry_live && (draw_tick % 30) == 0)
            send_web_json(&fl);
        if (lora_silence_ticks > 150)
        {
            fl.lora_ok = 0;
            fl.gps_fix = 0;
            fl.sat = 0;
            fl.time_str[0] = '\0';
            telemetry_live = 0;
            lora_dirty = 1;
        }
        frame++;
        if (frame % 4 == 0) hist_idx = (hist_idx + 1) % HIST_N;
        v_hist[hist_idx] = fl.v;
        a_hist[hist_idx] = fl.az - 1.0f;
        alt_hist[hist_idx] = fl.alt;
        pit_hist[hist_idx] = fl.pitch;
        rol_hist[hist_idx] = fl.roll;
        yaw_hist[hist_idx] = fl.yaw;
        ax_hist[hist_idx] = fl.ax;
        ay_hist[hist_idx] = fl.ay;
        az_hist[hist_idx] = fl.az;

        if (touch_fd >= 0)
        {
            int ret, got_touch = 0;
            /* 一轮排空触摸队列，避免绘图期间积压导致滑动延迟 */
            while ((ret = read(touch_fd, &sample, sizeof(sample))) > 0)
            {
                if (sample.npoints <= 0) continue;
                got_touch = 1;
                int tx = sample.point[0].x;
                if (!touch_down)
                {
                    touch_down = 1;
                    swipe_start_x = tx;
                    swiped = 0;
                    /* 雷达页：点在“校准地基”按钮上 → 用箭载坐标重设地基 */
                    if (cur_page == 5 && radar_btn_x >= 0)
                    {
                        int ty = sample.point[0].y;
                        if (tx >= radar_btn_x && tx <= radar_btn_x + radar_btn_w &&
                            ty >= radar_btn_y && ty <= radar_btn_y + radar_btn_h)
                        {
                            ground_set_from_rocket(&fl);
                            lora_dirty = 1;
                            swiped = 1;
                        }
                    }
                }
                if (!swiped)
                {
                    int dx = tx - swipe_start_x;
                    if (dx < -50)
                    {
                        if (cur_page < NUM_PAGES - 1) cur_page++;
                        swiped = 1;
                    }
                    else if (dx > 50)
                    {
                        if (cur_page > 0) cur_page--;
                        swiped = 1;
                    }
                }
            }
            if (!got_touch)
            {
                touch_down = 0;
                swipe_start_x = -1;
                swiped = 0;
            }
            else
            {
                page_auto_tick = 0;  /* 有触摸则暂停自动轮播，让用户手动控制 */
            }
        }

        /* 不自动轮播：固定显示状态页，避免页面自动切换后误以为 LCD 全红。
         * 通过触摸左右滑动手动切换页面，切页时仍立即重绘。 */
        page_auto_tick = 0;

        /* 触摸高频轮询，绘图降频；切页时立即重绘，避免页面绘制拖住触摸 */
        draw_tick++;
        /* LCD 全屏传输受面板总线带宽限制，稳定在约25~30帧/秒，避免整屏变色。 */
        if (cur_page != prev_page || (draw_tick % 6) == 0)
        {
            fill_rect(0, 0, fb_w, fb_h, C_BLACK);
            prev_page = cur_page;

            switch (cur_page)
            {
                case 0: draw_page_sat(&fl);  break;
                case 1: draw_page_curves(&fl); break;   /* 曲线页放第2页，开机8秒即可看到 */
                case 2: draw_page_vel(&fl);  break;
                case 3: draw_page_att(&fl);  break;
                case 4: draw_page_rocket_big(&fl); break;
                case 5: draw_page_radar(&fl); break;
            }

            /* 底部提示 */
            text(SAFE_X + 60, fb_h - SAFE_Y - 4, "< SWIPE >", C_LABEL);
            fb_update();
            lora_dirty = 0;
            if (draw_tick < 4)
                printf("DRAW page=%d ok\n", cur_page);
        }
        usleep(5000);
    }

    close(touch_fd);
    munmap(fb, pinfo.fblen);
    close(fb_fd);
    return 0;
}

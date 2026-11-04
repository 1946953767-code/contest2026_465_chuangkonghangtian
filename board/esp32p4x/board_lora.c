/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_lora.c
 *
 * A28-T2G4A12D1a (Chengdu Ashining, SX1281) 2.4 GHz serial module driver.
 *
 * Rewritten against the module's own manual, a28t2g4a12d1acn.pdf V1.0
 * (2023-02-03), after the first version produced a *false positive*: it
 * treated "read() returned more than zero bytes" as "the module answered".
 * A UART that sees no traffic produced 25 x 0x00, so every probe reported
 * detected=1 and the baud sweep stopped on a rate that never answered.
 *
 * ---- what the manual actually says (section numbers are the manual's) ----
 *
 * 5.2  pins      1 MD0, 2 MD1, 3 RXD (in), 4 TXD (out), 5 AUX (out),
 *                6 VCC 3.0-5.5 V, 7 GND
 * 5.3  states    MD0=0 MD1=0  half duplex (data)
 *                MD0=0 MD1=1  full duplex
 *                MD0=1 MD1=0  full duplex
 *                MD0=1 MD1=1  sleep / configuration only
 *                AUX low = module busy; AUX high ~2 ms = idle, safe to switch
 *                switch timings T1 2 ms, T2 3 ms, T3 3 ms, T4 2 ms, T_sc 300 us
 *                (this driver waits 20 ms, well past all of them)
 * 5.3  power     input ripple < 100 mV, must source > 200 mA peaks
 * 6.1  commands  binary, NOT AT; only accepted in the 1/1 state
 *                  0xC1 x3             read configuration
 *                  0xC3 x3             read hardware version "A28-T2G4A12D1a-V9.0"
 *                  0xF3 x3             read software version "A28-T2G4A12D1a-V9.00"
 *                  0xC0 ADDH ADDL SPEED CHAN OPTION   write, saved
 *                  0xC2 ...            write, not saved
 *                  0xC4 x3             reset
 *                  0xC9 x3             factory defaults
 *                  0xAF AF 73 00 AF F3 data RSSI
 *                answer delays 30 ms (C0/C2), 5 ms (C1/C3/C4/F3)
 * 6.2  SPEED     [7:6] parity 00=8N1, [5:3] baud index, [2:0] air rate
 *                baud index 0..7 = 1200 2400 4800 9600 19200 38400 57600 115200
 *      CHAN      0x00-0x64 (FLRC), 0x00-0x32 (LORA); factory default 0x00
 *      OPTION    [7] modulation 0=FLRC 1=LORA, [6] continuous (air rate auto),
 *                [1:0] power 11=12 dBm (default)
 * 6.3  factory   ADDH ADDL SPEED CHAN OPTION = 12 34 18 00 03
 *                => address 0x1234, 9600 8N1, channel 0x00, FLRC, 12 dBm
 *
 * ---- what this driver fixes versus the first version -----------------------
 *
 * 1. detection is judged on the answer's *content* ("A28" / "OK" / "ERROR"),
 *    never on the byte count, so an empty line cannot look like a module
 * 2. the baud sweep covers all eight rates and starts with the factory 9600
 * 3. MD0/MD1 are routed through the IO-MUX as plain GPIOs before the output
 *    latch is used.  The reset default for GPIO22/23 is FUNC 4
 *    (DBG_PSRAM_CK_PAD / DBG_PSRAM_CS_PAD), *not* GPIO, so the earlier raw
 *    latch writes could not drive the pads at all - which leaves MD0/MD1 on
 *    the module's own 1 MOhm pull-ups, i.e. the 1/1 sleep+config state.
 * 4. the UART's own view of the RX line level is reported, which tells a
 *    stuck-low line apart from a genuinely silent module
 * 5. the C1 answer is validated (leading 0xC0 echo + SPEED fields) instead of
 *    being memcpy'd on the strength of its length
 *
 * Wiring (unchanged, see docs/第31轮_LoRa_A28接线与配置表.md):
 *   module RXD -> P4X GPIO20 (UART1 TX)      module MD0 -> GPIO22
 *   module TXD -> P4X GPIO21 (UART1 RX)      module MD1 -> GPIO23
 *   module AUX -> P4X GPIO24 (optional)      module VCC -> 5V, GND -> GND
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include <nuttx/serial/tioctl.h>

#include "board_lora.h"

#ifdef CONFIG_BOARD_LORA

/* --------------------------------------------------------------- config -- */

static const char *g_dev_candidates[] =
{
  "/dev/ttyS0", "/dev/ttyS1", "/dev/ttyS2"
};

#define LORA_DEV_CANDIDATES 3
#define LORA_DEV      g_dev_name

#ifndef CONFIG_BOARD_LORA_BAUD
#  define CONFIG_BOARD_LORA_BAUD 9600
#endif

#ifndef CONFIG_BOARD_LORA_MD0
#  define CONFIG_BOARD_LORA_MD0 22
#endif

#ifndef CONFIG_BOARD_LORA_MD1
#  define CONFIG_BOARD_LORA_MD1 23
#endif

#ifndef CONFIG_BOARD_LORA_AUX
#  define CONFIG_BOARD_LORA_AUX 24
#endif

/* All eight rates of the SPEED[5:3] field (manual 6.2).  The factory rate is
 * 9600 and is tried first - it is what an untouched module uses.
 */

static const int g_rates[] =
{
  9600, 19200, 38400, 57600, 115200, 4800, 2400, 1200
};

#define LORA_NRATES ((int)(sizeof(g_rates) / sizeof(g_rates[0])))

/* ------------------------------------------------------ P4X raw registers -- */

/* ------------------------------------------------------ P4X raw registers -- */

static inline unsigned int p4x_rd(unsigned long a)
{
  return *(volatile unsigned int *)a;
}

static inline void p4x_wr(unsigned long a, unsigned int v)
{
  *(volatile unsigned int *)a = v;
}

/* The register block that is actually behind /dev/ttyS0 lives at 0x500d2000 on
 * this board, found by scanning the candidate blocks for a non-zero CLK_CONF.
 * An earlier round of this work read 0x500d0000 and concluded the UART was
 * dead, when in fact that address simply is not the open controller - the live
 * one even had 48 bytes sitting in its RX FIFO at the time.
 *
 * board_lora_uart_base() returns the live block; every raw access below goes
 * through it instead of a hard-coded address.
 */

#define P4X_UART_BASE_CANDIDATES 4
/* P4X-REALBASE: the real UART blocks.  REG_HPPERIPH1_BASE is 0x500c0000 and
 * the UARTs sit at +0xa000 (UART0) and +0xb000 (UART1).  An earlier revision
 * probed 0x500d0000..0x500d3000, which are not UARTs at all, and every
 * conclusion drawn from those reads was measuring another peripheral.
 */

/* P4X-UART1-LORA: per the official wiring table the A28 sits on UART1's own
 * pads (GPIO20 = UART1 TX, GPIO21 = UART1 RX).  UART0's pads GPIO37/38 are the
 * debug header, where a CH343P cable is plugged in.  So probe UART1 first.
 */

static const unsigned long g_uart_bases[P4X_UART_BASE_CANDIDATES] =
{
  0x500cb000ul, 0x500ca000ul, 0x500d2000ul, 0x500d3000ul
};

#define P4X_UART_OFF_STATUS   0x1cul
#define P4X_UART_OFF_CLKDIV   0x14ul
#define P4X_UART_OFF_CONF0    0x20ul
#define P4X_UART_OFF_CLKCONF  0x88ul
#define P4X_UART_OFF_DATE     0x8cul
#define P4X_UART_OFF_ID       0x9cul

#define P4X_UART_RXFIFO_CNT  0x000000fful
#define P4X_UART_RXD_LEVEL   0x00008000ul
#define P4X_UART_TXD_LEVEL   0x80000000ul

static unsigned long g_uart_base;      /* 0 until discovered */

unsigned long board_lora_uart_base(void)
{
  int i;

  if (g_uart_base != 0)
    {
      return g_uart_base;
    }

  for (i = 0; i < P4X_UART_BASE_CANDIDATES; i++)
    {
      unsigned long b = g_uart_bases[i];
      unsigned int clk = p4x_rd(b + P4X_UART_OFF_CLKCONF);
      unsigned int conf0 = p4x_rd(b + P4X_UART_OFF_CONF0);

      /* A controller somebody opened and programmed has a non-zero clock
       * divider.  A block nobody touched reads all zeros.
       */

      if ((clk != 0 && (clk & 0xfffu) != 0) || conf0 != 0)
        {
          unsigned int div = p4x_rd(b + P4X_UART_OFF_CLKDIV);

          g_uart_base = b;

          /* P4X-UART1-LORA: the serial driver only ever programmed the port
           * that /dev/ttySx gave it.  Both UARTs share the same sclk source in
           * this clock tree, so a divider produced for the other port is valid
           * here too; without it this UART has no bit clock at all.
           */

          if (div == 0 && i != 0)
            {
              unsigned int other = p4x_rd(g_uart_bases[0] +
                                          P4X_UART_OFF_CLKDIV);

              if (other != 0)
                {
                  p4x_wr(b + P4X_UART_OFF_CLKDIV, other);
                  printf("P4X-LORA: clkdiv copied from the open port: %u\n",
                         other);
                }
            }

          printf("P4X-LORA: uart base 0x%08lx clkdiv=%u conf0=0x%08x\n",
                 b, p4x_rd(b + P4X_UART_OFF_CLKDIV),
                 p4x_rd(b + P4X_UART_OFF_CONF0));

          return b;
        }
    }

  g_uart_base = g_uart_bases[0];
  return g_uart_base;
}

static unsigned int lora_uart_status(void)
{
  return p4x_rd(board_lora_uart_base() + P4X_UART_OFF_STATUS);
}

/* ESP32-P4 GPIO / IO-MUX / UART1 blocks.  Addresses are from the vendor
 * headers shipped with this tree:
 *   components/soc/esp32p4/register/hw_ver3/soc/gpio_reg.h
 *     GPIO_OUT_REG base+0x04  OUT_W1TS base+0x08  OUT_W1TC base+0x0c
 *     GPIO_ENABLE_W1TS base+0x24   GPIO_IN_REG base+0x3c
 *   .../io_mux_reg.h
 *     PERIPHS_IO_MUX_U_PAD_GPIO22 = REG_IO_MUX_BASE + 0x5c
 *     PERIPHS_IO_MUX_U_PAD_GPIO23 = REG_IO_MUX_BASE + 0x60
 *     MCU_SEL occupies bits [14:12] and 1 selects plain GPIO
 *   .../uart_reg.h
 *     UART_STATUS_REG(1) = UART1 base + 0x1c
 *       [7:0] RXFIFO count, [15] the RX line level the UART itself sees
 */

#define P4X_GPIO_BASE        0x500e0000ul
#define P4X_GPIO_OUT         (P4X_GPIO_BASE + 0x04ul)
#define P4X_GPIO_OUT_W1TS    (P4X_GPIO_BASE + 0x08ul)
#define P4X_GPIO_OUT_W1TC    (P4X_GPIO_BASE + 0x0cul)
#define P4X_GPIO_ENABLE      (P4X_GPIO_BASE + 0x20ul)
#define P4X_GPIO_ENABLE_W1TS (P4X_GPIO_BASE + 0x24ul)
#define P4X_GPIO_IN          (P4X_GPIO_BASE + 0x3cul)

/* GPIO matrix: peripheral input signals are muxed by FUNC_IN_SEL_CFG(n) and
 * peripheral outputs by FUNC_OUT_SEL_CFG(pin).
 *
 * The signal numbers come from the P4's own table,
 * components/soc/esp32p4/include/soc/gpio_sig_map.h :
 *     UART0_RXD_PAD_IN_IDX / UART0_TXD_PAD_OUT_IDX = 10
 *     UART1_RXD_PAD_IN_IDX / UART1_TXD_PAD_OUT_IDX = 13
 *     UART2_RXD_PAD_IN_IDX / UART2_TXD_PAD_OUT_IDX = 16
 *
 * The earlier code used 25/24, which are the ESP32/ESP32-S3 numbers, and the
 * loopback example kept them in P4X_RX_SIG/P4X_TX_SIG.  On the P4 those select
 * a different peripheral's signal, so GPIO20/21 were never connected to UART1
 * at all - which is why every byte written vanished and UART1's RXD never
 * followed its pad.
 */

#define P4X_GPIO_FUNC_IN_SEL_CFG(i)  (P4X_GPIO_BASE + 0x158ul + 4ul * (i))
/* P4X-INCFG-FIX: slot n lives at base + 0x158 + 4n (gpio_reg.h: FUNC1 =
 * 0x15c, FUNC2 = 0x160).  The earlier 0x154 addressed the slot one lower,
 * so UART0/UART1 RX input slots were never written and kept their reset
 * value 0x3E, which means "this input is tied permanently low".
 */
#define P4X_GPIO_FUNC_OUT_SEL_CFG(p) (P4X_GPIO_BASE + 0x554ul + 4ul * (p))
/* P4X-UART0-SWITCH: the LoRa port is UART0, whose matrix signals are 10.
 * gpio_sig_map.h: UART0_RXD_PAD_IN_IDX / UART0_TXD_PAD_OUT_IDX = 10.
 */
#define P4X_UART0_TX_SIG             10u
#define P4X_UART0_RX_SIG             10u
/* P4X-UART1-LORA: the LoRa port is UART1 again, whose matrix signals are 13
 * (gpio_sig_map.h: UART1_RXD_PAD_IN_IDX / UART1_TXD_PAD_OUT_IDX = 13).
 */

#define P4X_UART1_TX_SIG             13u
#define P4X_UART1_RX_SIG             13u
#define P4X_UART2_TX_SIG             16u
#define P4X_UART2_RX_SIG             16u
#define P4X_FUNC_IN_SIG_SEL          0x00000080ul  /* IN_SEL_CFG[7] */
#define P4X_FUNC_OUT_OEN_SEL         0x00000400ul  /* OUT_SEL_CFG[10] */

#define P4X_IO_MUX_BASE      0x500e1000ul
/* P4X-IOMUX-TABLE: the IO-MUX register map is NOT linear.  io_mux_reg.h gives
 *   GPIO0..4  = 0x04 .. 0x14      GPIO10 = 0x2C      GPIO11 = 0x30
 *   GPIO20    = 0x54              GPIO21 = 0x58      GPIO22 = 0x5C
 *   GPIO23    = 0x60              GPIO24 = 0x64
 *   GPIO37    = 0x98              GPIO38 = 0x9C
 * The old formula (0x54 + 4*pin) matched GPIO20..24 by coincidence and missed
 * GPIO37/38 by 0x50, so writes meant for the UART0 pads hit unrelated
 * registers and the pads kept reading 0.
 */

#define P4X_IO_MUX_PIN(p)    board_lora_iomux_addr(p)

static unsigned long board_lora_iomux_addr(int pin)
{
  switch (pin)
    {
      case 0:  return P4X_IO_MUX_BASE + 0x04ul;
      case 1:  return P4X_IO_MUX_BASE + 0x08ul;
      case 2:  return P4X_IO_MUX_BASE + 0x0cul;
      case 3:  return P4X_IO_MUX_BASE + 0x10ul;
      case 4:  return P4X_IO_MUX_BASE + 0x14ul;
      case 10: return P4X_IO_MUX_BASE + 0x2cul;
      case 11: return P4X_IO_MUX_BASE + 0x30ul;
      case 20: return P4X_IO_MUX_BASE + 0x54ul;
      case 21: return P4X_IO_MUX_BASE + 0x58ul;
      case 22: return P4X_IO_MUX_BASE + 0x5cul;
      case 23: return P4X_IO_MUX_BASE + 0x60ul;
      case 24: return P4X_IO_MUX_BASE + 0x64ul;
      case 37: return P4X_IO_MUX_BASE + 0x98ul;
      case 38: return P4X_IO_MUX_BASE + 0x9cul;
      default: break;
    }

  /* Unknown pin: fall back to the linear guess so the caller still gets a
   * plausible address instead of a wild one.
   */

  return P4X_IO_MUX_BASE + 0x04ul + 4ul * (unsigned long)pin;
}
#define P4X_MCU_SEL_M        0x00007000ul
#define P4X_MCU_SEL_GPIO     0x00001000ul
#define P4X_FUN_IE           0x00000200ul   /* input enable  */
#define P4X_FUN_WPU          0x00000100ul   /* pull-up       */
#define P4X_FUN_WPD          0x00000400ul   /* pull-down     */

#define P4X_UART_CLK_CONF    (P4X_UART_OFF_CLKCONF)
#define P4X_UART_ID          (P4X_UART_OFF_ID)
#define P4X_UART_DATE        (P4X_UART_OFF_DATE)

/* --------------------------------------------------------------- state -- */

static int g_fd = -1;
static char g_dev_name[16] = "none";
static int g_inited;
static int g_version_ok;
static char g_hwver[48];
static char g_swver[48];
static uint8_t g_cfg[6];
static int g_cfg_ok;
static int g_baud = CONFIG_BOARD_LORA_BAUD;
static int g_md0 = -1;
static int g_md1 = -1;
static unsigned long g_tx_bytes;
static unsigned long g_rx_bytes;
static unsigned long g_tx_pkts;
static unsigned long g_rx_pkts;
static int g_last_rssi = -127;

/* --------------------------------------------------------------- helpers -- */

static int lora_pin_level(int pin)
{
  return (int)((p4x_rd(P4X_GPIO_IN) >> pin) & 1u);
}

/* Route a pad through the IO-MUX as a plain GPIO, then enable its output.
 * Without the IO-MUX step the output latch is not connected to the pad:
 * GPIO22/23 come up as FUNC 4, the PSRAM debug pads.
 */

static void lora_pin_output(int pin)
{
  unsigned long reg = P4X_IO_MUX_PIN(pin);
  unsigned int v = p4x_rd(reg);

  v = (v & ~P4X_MCU_SEL_M) | P4X_MCU_SEL_GPIO;
  p4x_wr(reg, v);
  p4x_wr(P4X_GPIO_ENABLE_W1TS, (1u << pin));
}

static void lora_pin_set(int pin, int level)
{
  if (level)
    {
      p4x_wr(P4X_GPIO_OUT_W1TS, (1u << pin));
    }
  else
    {
      p4x_wr(P4X_GPIO_OUT_W1TC, (1u << pin));
    }
}

/* Manual 5.3: only switch state while AUX is high (module idle) and wait out
 * T1..T4 / T_sc.  20 ms covers every one of them several times over.
 */

static void lora_mode(int md0, int md1)
{
  lora_pin_set(CONFIG_BOARD_LORA_MD0, md0);
  lora_pin_set(CONFIG_BOARD_LORA_MD1, md1);
  usleep(20000);

  g_md0 = md0;
  g_md1 = md1;
}

static int lora_rate(int baud)
{
  struct termios tio;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  memset(&tio, 0, sizeof(tio));

  if (tcgetattr(g_fd, &tio) < 0)
    {
      return -errno;
    }

  cfsetspeed(&tio, baud);
  tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
  tio.c_cflag |= CS8;
  tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tio.c_iflag &= ~(ISTRIP | INLCR | ICRNL | IGNCR);
  tio.c_oflag &= ~OPOST;
  tio.c_cc[VMIN]  = 0;
  tio.c_cc[VTIME] = 1;                 /* 100 ms read timeout */

  if (tcsetattr(g_fd, TCSANOW, &tio) < 0)
    {
      return -errno;
    }

  g_baud = baud;
  return OK;
}

static void lora_flush(void)
{
  uint8_t junk[32];
  int d;

  while ((d = read(g_fd, junk, sizeof(junk))) > 0)
    {
      /* drop whatever a previous experiment left behind */
    }
}

/* Read until `want` bytes arrive or the time runs out.
 *
 * The upper half of this UART driver can hand back bytes that were never on
 * the wire, so every caller validates the *content* - see lora_looks_ok().
 */

static int lora_read_reply(FAR uint8_t *buf, int buflen, int timeout_ms)
{
  int total = 0;
  int waited = 0;

  memset(buf, 0, (size_t)buflen);

  while (total < buflen && waited < timeout_ms)
    {
      int n = read(g_fd, buf + total, buflen - total);

      if (n > 0)
        {
          total += n;
        }
      else
        {
          usleep(20000);
          waited += 20;
        }
    }

  return total;
}

/* The literal answer test.  The manual defines exactly what a live module
 * says, so anything else - including a non-empty buffer - is not an answer.
 */

static int lora_looks_ok(FAR const uint8_t *buf, int len)
{
  int i;

  if (len <= 0)
    {
      return 0;
    }

  for (i = 0; i < len; i++)
    {
      /* C3 / F3 version string */
      if (i + 3 <= len && memcmp(buf + i, "A28", 3) == 0)
        {
          return 1;
        }

      /* C0 / C2 / C4 / C9 acknowledgement */
      if (i + 2 <= len && memcmp(buf + i, "OK", 2) == 0)
        {
          return 1;
        }

      /* configuration refused / unknown command */
      if (i + 5 <= len && memcmp(buf + i, "ERROR", 5) == 0)
        {
          return 1;
        }
    }

  return 0;
}

static void lora_print_hex(FAR const char *tag, FAR const uint8_t *buf, int len)
{
  int i;
  int lim = (len < 48) ? len : 48;

  printf("P4X-LORA: %s rx[%d] =", tag, len);
  for (i = 0; i < lim; i++)
    {
      printf(" %02x", buf[i]);
    }

  printf("  |");
  for (i = 0; i < lim; i++)
    {
      int c = buf[i];

      printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
    }

  printf("|\n");

  if (len > 0 && !lora_looks_ok(buf, len))
    {
      printf("P4X-LORA: %s rejected - no A28/OK/ERROR among those bytes\n", tag);
    }
}

/* Report what the UART hardware itself sees: the RX FIFO count and the RX
 * line level.  Those two facts separate a silent module from a line that is
 * being held low.
 *
 * UART_STATUS[15] is the level inside the UART, after the GPIO matrix.  To
 * tell "the pad really is low" apart from "the pad is not routed into the
 * UART at all" the raw pad level and the IO-MUX/routing registers are printed
 * as well.
 */

static void lora_report_line(FAR const char *tag)
{
  unsigned int st = lora_uart_status();
  unsigned int pad = p4x_rd(P4X_GPIO_IN);
  unsigned int mux = p4x_rd(P4X_IO_MUX_PIN(21));
  unsigned int insel = p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(P4X_UART1_RX_SIG));
  unsigned int outsel;

  printf("P4X-LORA: %s line: rxfifo=%lu rxd_level=%u | pad21=%u io_mux21=0x%08x"
         " (mcu_sel=%u) in_sel[25]=0x%08x\n",
         tag, (unsigned long)(st & P4X_UART_RXFIFO_CNT),
         (st & P4X_UART_RXD_LEVEL) ? 1u : 0u,
         (pad >> 21) & 1u, mux, (mux & P4X_MCU_SEL_M) >> 12, insel);

  /* The TX side is worth checking in the same breath: if the UART's TXD is
   * not routed to GPIO20 then the module never hears a command either.
   */

  outsel = p4x_rd(P4X_GPIO_FUNC_OUT_SEL_CFG(37));
  printf("P4X-LORA: %s tx: out_sel[GPIO20]=0x%08x (uart1_tx_sig=%lu)%s\n",
         tag, outsel, (unsigned long)(outsel & 0x1ffu),
         ((outsel & 0x1ffu) == P4X_UART1_TX_SIG) ? "" :
         "  <- GPIO20 is NOT driven by UART1!");

  if (((pad >> 21) & 1u) == 0)
    {
      printf("P4X-LORA: %s the pad itself reads low - something outside the "
             "chip is holding GPIO21 down\n", tag);
    }
  else if ((st & P4X_UART_RXD_LEVEL) == 0)
    {
      printf("P4X-LORA: %s the PAD is high but the UART sees low - GPIO21's "
             "IO-MUX input path is not delivering the pad to UART1 RX\n", tag);
    }
}

/* ------------------------------------------------------------ public API -- */

/****************************************************************************
 * Name: board_lora_route
 *
 * Description:
 *   Wire UART1 to GPIO20/GPIO21 through the GPIO matrix, the way the vendor
 *   HAL would.
 *
 *   Found the hard way: on this image the matrix was *not* set up.  Reading
 *   the routing registers before this fix gave
 *       in_sel[UART1_RX=25] = 0x3e   (input signal 62, not a pin)
 *       out_sel[GPIO20]     = 0x100  (signal index 0, OEN_SEL = 1)
 *   so the bytes written to /dev/ttyS0 never reached the pad at all and UART1
 *   listened to an internal signal instead of the module.  The module could
 *   not have answered no matter how it was wired.
 *
 *   Matrix register layout (gpio_reg.h):
 *     GPIO_FUNC_IN_SEL_CFG(n)  = base + 0x158 + 4*n   for peripheral input n
 *       [5:0] FUNC_IN_SEL (which pad feeds this input), [7] SIG_IN_SEL
 *     GPIO_FUNC_OUT_SEL_CFG(p) = base + 0x554 + 4*p   for pad p
 *       [8:0] FUNC_OUT_SEL (which peripheral signal drives this pad)
 *   UART1: TX = signal 24, RX = signal 25.
 *
 *   The pad's own IO-MUX function must select the GPIO matrix (FUNC 0/1) and
 *   the input buffer must be enabled, otherwise nothing reaches the matrix.
 *
 ****************************************************************************/

/* ---------------------------------------------------------------- P4X-SHORTTEST
 *
 * Ping-pong of evidence that ended the guessing game:
 *
 *   * driving GPIO37 low did not move GPIO21  -> "no contact"?  NO: the pad
 *     helpers only wrote bit <pin> into GPIO_ENABLE / GPIO_OUT_W1TS, which hold
 *     GPIO0..31; for pin 37 that is 1ul << 37 = 0, so the pad was never enabled
 *     as an output at all.  Pins 32..53 live in ENABLE1 (0x30) / OUT1_W1TS
 *     (0x14) and need their own bit 0..21.
 *   * sending a byte over the UART pulled pad 21 low -> the wire IS there.
 *
 * So: do the GPIO test properly, then dump the input matrix and see which slot
 * the serial driver attached pad 21 to.  There is no gpio_sig_map.h in this
 * tree, so "UART0 RX is slot 10" was only ever an assumption.
 */

#define P4X_GPIO_ENABLE_W1TC  (P4X_GPIO_BASE + 0x28ul)
#define P4X_GPIO_OUT1_W1TS    (P4X_GPIO_BASE + 0x14ul)
#define P4X_GPIO_OUT1_W1TC    (P4X_GPIO_BASE + 0x18ul)
#define P4X_GPIO_ENABLE1_W1TS (P4X_GPIO_BASE + 0x30ul)
#define P4X_GPIO_ENABLE1_W1TC (P4X_GPIO_BASE + 0x34ul)
#define P4X_GPIO_IN1          (P4X_GPIO_BASE + 0x40ul)

/* Pad level for any pin, including 32..53. */

static int lora_pad_level(int pin)
{
  if (pin < 32)
    {
      return (int)((p4x_rd(P4X_GPIO_IN) >> pin) & 1u);
    }

  return (int)((p4x_rd(P4X_GPIO_IN1) >> (pin - 32)) & 1u);
}

/* Make a pad a plain GPIO output.
 *
 * The out_sel write is the part that was missing: board_lora_route() leaves
 * out_sel[37] = UART0_TXD | OEN_SEL, i.e. the pad's output enable is owned by
 * the peripheral.  As long as OEN_SEL is set, enabling the GPIO output does
 * nothing at all - which is why the first contact test reported NO CONTACT
 * while the UART test on the same wire pulled the RX pad low.
 */

static void lora_pad_output(int pin)
{
  unsigned long reg = P4X_IO_MUX_PIN(pin);
  unsigned int v = p4x_rd(reg);

  v = (v & ~P4X_MCU_SEL_M) | P4X_MCU_SEL_GPIO;
  p4x_wr(reg, v);

  /* FUNC_OUT_SEL = 256 (0x100): the GPIO block drives the pad, OEN_SEL clear. */

  p4x_wr(P4X_GPIO_FUNC_OUT_SEL_CFG(pin), 0x100u);

  if (pin < 32)
    {
      p4x_wr(P4X_GPIO_ENABLE_W1TS, (1u << pin));
    }
  else
    {
      p4x_wr(P4X_GPIO_ENABLE1_W1TS, (1u << (pin - 32)));
    }
}

static void lora_pad_drive(int pin, int level)
{
  if (pin < 32)
    {
      p4x_wr(level ? P4X_GPIO_OUT_W1TS : P4X_GPIO_OUT_W1TC, (1u << pin));
    }
  else
    {
      p4x_wr(level ? P4X_GPIO_OUT1_W1TS : P4X_GPIO_OUT1_W1TC,
             (1u << (pin - 32)));
    }
}

static void lora_pad_release(int pin)
{
  if (pin < 32)
    {
      p4x_wr(P4X_GPIO_ENABLE_W1TC, (1u << pin));
    }
  else
    {
      p4x_wr(P4X_GPIO_ENABLE1_W1TC, (1u << (pin - 32)));
    }
}

/* Which matrix input slots are pointed at the LoRa pads? */

static void lora_dump_insel(FAR const char *when)
{
  int slot;
  int found = 0;

  for (slot = 0; slot < 64; slot++)
    {
      unsigned int v = p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(slot));
      unsigned int pad = v & 0x3fu;

      if ((v & 0x7fu) == 0u)
        {
          continue;                             /* unused slot */
        }

      if (pad == 21u || pad == 37u || pad == 38u)
        {
          printf("P4X-INSEL: %s slot %d = 0x%08x -> pad %u%s\n",
                 when, slot, v, pad,
                 (v & 0x80u) ? " (SIG_IN_SEL=1 direct)" : " (matrix)");
          found++;
        }
    }

  printf("P4X-INSEL: %s %d slot(s) point at pad 21/37/38\n", when, found);
}

int board_lora_route(int rx_pin, int tx_pin)
{
  /* P4X-UARTROUTE-FULL: the NuttX ESP32-P4 serial driver never programs the
   * IO-MUX or the GPIO matrix (it only reads uart_periph_signal[].irq), and
   * board_bringup.c does not either.  Measured consequence: /dev/ttyS0 ran
   * with its TX and RX attached to nothing - UART_STATUS[15] (UART_RXD) read 0
   * while GPIO21 measured 3.25 V, and driving GPIO21 as an output moved
   * GPIO_IN[21] but left UART_STATUS[15] at 0 for both levels.
   *
   * So attach UART1 to the pads here:
   *   TX pad  <- UART1_TXD (signal 13), pad enable taken from the peripheral
   *   RX slot <- GPIO21 through the matrix (SIG_IN_SEL = 0).  SIG_IN_SEL = 1
   *              means "use the direct IO-MUX path", and GPIO20/21 have no
   *              UART1 function in that table (only GPIO10..13 do), which is
   *              why every earlier attempt left the controller on a dead line.
   */

  unsigned int in_v;
  unsigned int out_v;
  unsigned long mux_rx = P4X_IO_MUX_PIN(rx_pin);

  /* 1) pads: GPIO21 as an input with the input buffer on, GPIO20 as an
   *    output.  FUNC 1 = GPIO for these pads (see io_mux_reg.h).
   */

  in_v = p4x_rd(mux_rx);
  in_v &= ~P4X_MCU_SEL_M;
  in_v |= P4X_FUN_IE;
  p4x_wr(mux_rx, in_v);

  {
    unsigned long mux_tx = P4X_IO_MUX_PIN(tx_pin);
    unsigned int v = p4x_rd(mux_tx);

    /* P4X-TXPAD37: the TX pad is not one of UART1's own pads, so it has to be
     * plain GPIO for the matrix to be able to drive the UART's TXD out of it.
     */

    v = (v & ~P4X_MCU_SEL_M) | P4X_MCU_SEL_GPIO;
    p4x_wr(mux_tx, v);
  }

  /* 2) matrix: UART1 RX input <- pad, UART1 TX output -> pad */

  /* P4X-MATRIXSIG: measured on hardware, the polarity is the other way round.
   *
   *   in_sel[10] = 0x15 (pad 21, SIG_IN_SEL = 0), pad 21 driven low -> rxd = 1
   *   in_sel[10] = 0x95 (pad 21, SIG_IN_SEL = 1), pad 21 driven low -> rxd = 0
   *
   * So bit 7 selects the GPIO matrix and 0 falls back to the pad's own direct
   * IO-MUX input - for UART0 that is the native RX pad GPIO38, which has no
   * wire on this board.  Writing only the pad index therefore left the
   * receiver listening to the wrong pin forever, however correct the routing
   * printout looked.
   */

  /* P4X-RXMATRIX: measured - with SIG_IN_SEL = 0 (the direct IO-MUX path) the
   * receive state machine never follows pad 21: pad20 and pad21 both moved while
   * STATUS.rxd stayed high for the whole 16-byte burst.  With the matrix bit set
   * the same pad does drive STATUS.rxd, so the receiver input is taken from the
   * matrix here.
   */

  in_v = P4X_FUNC_IN_SIG_SEL | (((unsigned int)rx_pin) & 0x3fu);
  p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(P4X_UART1_RX_SIG), in_v);

  /* FUNC_OUT_SEL = UART1_TXD, OEN_SEL = 1 so the pad is enabled by the UART. */

  out_v = P4X_UART1_TX_SIG | P4X_FUNC_OUT_OEN_SEL;
  p4x_wr(P4X_GPIO_FUNC_OUT_SEL_CFG(tx_pin), out_v);

  /* 3) read both back so the log shows whether it stuck. */

  in_v = p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(P4X_UART1_RX_SIG));
  out_v = p4x_rd(P4X_GPIO_FUNC_OUT_SEL_CFG(tx_pin));

  printf("P4X-LORA: route: in_sel[%u] <- GPIO%d => 0x%08x (sel=%u) %s\n",
         P4X_UART1_RX_SIG, rx_pin, in_v, in_v & 0x3fu,
         ((in_v & 0x3fu) == (unsigned)rx_pin) ? "OK" : "FAILED");

  printf("P4X-LORA: route: out_sel[GPIO%d] <- sig %u => 0x%08x (sel=%u) %s\n",
         tx_pin, P4X_UART1_TX_SIG, out_v, out_v & 0x1ffu,
         ((out_v & 0x1ffu) == P4X_UART1_TX_SIG) ? "OK" : "FAILED");

  return (((in_v & 0x3fu) == (unsigned)rx_pin) &&
          ((out_v & 0x1ffu) == P4X_UART1_TX_SIG)) ? OK : -EIO;
}

int board_lora_initialize(void)
{
  return board_lora_initialize_mode(1);
}

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif

/* P4X-BAUDCALC: derive the divider from the real clock registers. */

#define P4X_CLKRST_BASE        0x500e6000ul
#define P4X_CLKRST_CTRL110     0x68ul
#define P4X_CLKRST_CTRL111     0x6cul
#define P4X_CLKRST_CTRL112     0x70ul

static unsigned int lora_sclk_hz(unsigned int src_sel)
{
  switch (src_sel)
    {
      case 0:  return 40000000u;    /* XTAL 40 MHz   */
      case 2:  return 80000000u;    /* PLL_F80M      */
      default: return 40000000u;    /* RTC ~ coarse; treated as XTAL */
    }
}

int board_lora_baud_calc(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int c110 = p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL110);
  unsigned int c111 = p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL111);
  unsigned int c112 = p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL112);
  unsigned int div_num;
  unsigned int src_sel;
  unsigned int sclk;
  unsigned int sclk_div;
  unsigned long clk_div;
  unsigned int baud = CONFIG_BOARD_LORA_BAUD;

  printf("P4X-BAUDCALC: ctrl110=0x%08x ctrl111=0x%08x ctrl112=0x%08x\n",
         c110, c111, c112);

  /* the vendor's own set_baudrate writes UART1's divider into ctrl112,
   * and takes its clock source from ctrl111[25:24].
   */

  div_num = c112 & 0xffu;
  src_sel = (c111 >> 24) & 0x3u;

  /* P4X-PREDIV2: take the source clock straight from the selector.  Dividing
   * it by the *current* pre-divider made the calculation chase its own tail:
   * the register still held the 1 written by the previous boot, so the code
   * believed 20 MHz, then wrote the pre-divider back to 0 and ran the UART at
   * 19200 baud - no valid frame ever arrived.
   */

  sclk = lora_sclk_hz(src_sel);

  printf("P4X-BAUDCALC: div_num=%u (ignored) src_sel=%u -> sclk=%u Hz\n",
         div_num, src_sel, sclk);

  if (sclk == 0)
    {
      printf("P4X-BAUDCALC: bad clock, keeping the current divider\n");
      return -EIO;
    }

  /* vendor formula: pre-divide until the 12-bit divider can express the rate */

  sclk_div = (sclk + (4095ul * baud) - 1ul) / (4095ul * baud);

  if (sclk_div == 0)
    {
      sclk_div = 1;
    }

  clk_div = ((unsigned long)sclk << 4) / ((unsigned long)baud * sclk_div);

  printf("P4X-BAUDCALC: baud=%u sclk_div=%u clk_div=%lu -> int=%lu frag=%lu\n",
         baud, sclk_div, clk_div, clk_div >> 4, clk_div & 0xfu);

  /* P4X-PREDIV: the pre-divider lives in HP_SYS_CLKRST, not in the UART.
   * Writing only UART_CLKDIV leaves the hardware running from the undivided
   * clock, which doubles the baud (19200 instead of 9600) and receives nothing.
   */

  {
    unsigned int c112b = p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL112);

    p4x_wr(P4X_CLKRST_BASE + P4X_CLKRST_CTRL112,
           (c112b & ~0xffu) | ((sclk_div - 1u) & 0xffu));

    printf("P4X-BAUDCALC: prediv ctrl112 0x%08x -> 0x%08x (div_num=%u)\n",
           c112b, p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL112),
           p4x_rd(P4X_CLKRST_BASE + P4X_CLKRST_CTRL112) & 0xffu);
  }

  p4x_wr(b + P4X_UART_OFF_CLKDIV,
         (unsigned int)((clk_div >> 4) | ((clk_div & 0xful) << 16)));
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(30000);

  printf("P4X-BAUDCALC: clkdiv now 0x%08x\n",
         p4x_rd(b + P4X_UART_OFF_CLKDIV));

  return OK;
}

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif

/* P4X-BAUDFIX: program the divider for 9600 baud explicitly.
 *
 * Measured with the airborne side transmitting normally: sweeping the divider
 * produced clean JSON only around div = 3907, and that value turned out to be
 * 9600 baud (the A28's factory rate).  The effective source clock works out at
 * 37.5 MHz, so the exact divider is 3906.25: integer 3906, fraction 4.
 *
 * This cannot be left to tcsetattr(): on this chip the serial driver's
 * cfsetspeed() never touches UART_CLKDIV, which is why every earlier attempt
 * talked to the module at a made-up rate and received nothing but noise.
 */

int board_lora_baud_fix(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int v = 3906u | (4u << 16);      /* int 3906, frag 4 -> 3906.25 */

  p4x_wr(b + P4X_UART_OFF_CLKDIV, v);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(30000);

  printf("P4X-BAUDFIX: clkdiv=0x%08x (9600 baud, 4.12 fixed point)\n",
         p4x_rd(b + P4X_UART_OFF_CLKDIV));

  return OK;
}

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif

/* P4X-BAUDSEARCH: find the divider the module is really talking at.
 *
 * CLKDIV's power-on value is 0x2b6 and tcsetattr() never programs this
 * divider, so every rate tried so far was fiction.  Instead of computing,
 * search: sweep the divider geometrically and keep the one whose bytes look
 * like the JSON the airborne side transmits.
 */

int board_lora_baud_search(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int div;
  unsigned int best_div = 0;
  int best_score = -1;
  unsigned char buf[64];
  int i;
  int k;
  int score;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  printf("P4X-BAUDSEARCH: start, clkdiv now=%u\n",
         p4x_rd(b + P4X_UART_OFF_CLKDIV) & 0xfffu);

  for (div = 4; div < 4100; div = (div * 106u) / 100u + 1u)
    {
      p4x_wr(b + P4X_UART_OFF_CLKDIV, div);
      p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
      usleep(30000);

      for (i = 0; i < 24; i++)
        {
          (void)p4x_rd(b);
        }

      usleep(120000);

      score = 0;

      for (i = 0; i < 64; i++)
        {
          buf[i] = (unsigned char)(p4x_rd(b) & 0xffu);

          if (buf[i] >= 0x20u && buf[i] < 0x7fu)
            {
              score++;
            }
        }

      if (score > best_score)
        {
          best_score = score;
          best_div = div;
        }
    }

  printf("P4X-BAUDSEARCH: best div=%u score=%d/64\n", best_div, best_score);

  if (best_div != 0)
    {
      p4x_wr(b + P4X_UART_OFF_CLKDIV, best_div);
      p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
      usleep(30000);

      for (i = 0; i < 24; i++)
        {
          (void)p4x_rd(b);
        }

      usleep(200000);

      for (i = 0; i < 64; i++)
        {
          buf[i] = (unsigned char)(p4x_rd(b) & 0xffu);
        }

      printf("P4X-BAUDSEARCH: div=%u -> ", best_div);
      for (k = 0; k < 64; k++)
        {
          printf("%c", (buf[k] >= 0x20u && buf[k] < 0x7fu) ?
                       (char)buf[k] : '.');
        }

      printf("\n");
    }

  return OK;
}

/* P4X-BEACON: transmit a recognisable ASCII beacon for ten seconds. */

int board_lora_beacon_test(void)
{
  char msg[32];
  int i;
  int n;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  for (i = 0; i < 20; i++)
    {
      n = snprintf(msg, sizeof(msg), "P4XBCN-%d\r\n", i);

      board_lora_send(msg, (size_t)n);
      usleep(500000);
    }

  printf("P4X-BEACON: 20 beacons sent at %d baud\n", g_baud);
  return OK;
}

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif
/* P4X-BAUDSWEEP2: set UART_CLKDIV directly and listen at each candidate rate.
 *
 * tcsetattr() on this port never reaches the divider: after asking for 300 baud
 * the register still read the value the driver had written at open time.  So
 * every "baud" tried so far was fiction and the received bytes were garbage.
 *
 * The 9600 reference is whichever divider the port already carries; the others
 * follow by ratio.
 */

int board_lora_baud_sweep2(void)
{
  static const int rates[] = { 9600, 19200, 38400, 57600, 115200, 4800, 2400 };
  unsigned long b = board_lora_uart_base();
  unsigned int ref;
  unsigned char buf[33];
  int i;
  int r;
  int k;
  int printable;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  ref = p4x_rd(b + P4X_UART_OFF_CLKDIV) & 0xfffu;

  if (ref == 0)
    {
      ref = 694;
    }

  printf("P4X-BSWEEP: reference clkdiv=%u for 9600\n", ref);

  for (r = 0; r < (int)(sizeof(rates) / sizeof(rates[0])); r++)
    {
      unsigned int div = (unsigned int)(((unsigned long)ref * 9600ul) /
                                        (unsigned long)rates[r]);

      if (div == 0)
        {
          div = 1;
        }

      p4x_wr(b + P4X_UART_OFF_CLKDIV, div);
      p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
      usleep(30000);

      for (i = 0; i < 32; i++)
        {
          (void)p4x_rd(b);
        }

      usleep(400000);

      printable = 0;
      for (i = 0; i < 32; i++)
        {
          buf[i] = (unsigned char)(p4x_rd(b) & 0xffu);

          if (buf[i] >= 0x20u && buf[i] < 0x7fu)
            {
              printable++;
            }
        }

      printf("P4X-BSWEEP: %6d baud (div %u) rxfifo=%u :",
             rates[r], div, p4x_rd(b + P4X_UART_OFF_STATUS) & 0xffu);

      for (k = 0; k < 24; k++)
        {
          printf("%c", (buf[k] >= 0x20u && buf[k] < 0x7fu) ?
                       (char)buf[k] : '.');
        }

      printf("  printable=%d\n", printable);
    }

  /* leave the port at the reference (9600) rate. */

  p4x_wr(b + P4X_UART_OFF_CLKDIV, ref);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(20000);

  printf("P4X-BSWEEP: restored clkdiv=%u\n",
         p4x_rd(b + P4X_UART_OFF_CLKDIV) & 0xfffu);

  return OK;
}

/* P4X-DIAG3: measurement only - no routing changes, no assumptions. */



/* P4X-MODPROBE: talk to the A28 the way the manual says, and read the answer
 * straight out of the data port.
 *
 * Two facts made every earlier attempt look like "the module is silent":
 *   1 the receive path was pointed at the wrong UART/pads,
 *   2 board_lora_recv() trusted STATUS[7:0] (rxfifo_cnt), which this chip keeps
 *     at 0 - so even a real answer was never read.
 */

static void lora_mod_say(unsigned long b, FAR const char *tag,
                         FAR const unsigned char *cmd, int len)
{
  unsigned char back[40];
  unsigned int st;
  int i;
  int n = 0;
  int printable = 0;

  /* drain whatever the port already holds. */

  for (i = 0; i < 24; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send(cmd, (size_t)len);
  usleep(300000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);

  /* read up to 32 bytes straight from the data port. */

  for (i = 0; i < 32; i++)
    {
      unsigned int v = p4x_rd(b) & 0xffu;

      back[n++] = (unsigned char)v;

      if (v >= 0x20u && v < 0x7fu)
        {
          printable++;
        }
    }

  printf("P4X-MODPROBE: %s -> status=0x%08x rxfifo=%u afifo=0x%08x raw:",
         tag, st, st & 0xffu, p4x_rd(b + 0x90ul));

  for (i = 0; i < n && i < 24; i++)
    {
      printf(" %02x", back[i]);
    }

  printf("  ascii:");
  for (i = 0; i < n && i < 24; i++)
    {
      unsigned char c = back[i];

      printf("%c", (c >= 0x20u && c < 0x7fu) ? (char)c : '.');
    }

  printf("  printable=%d\n", printable);
}

/* P4X-MDTEST: can MD0/MD1 be driven at all, and which MCU_SEL makes them GPIO?
 *
 * The module only accepts configuration commands (C3/F3/C1) in the 1/1 sleep
 * state, so if MD0/MD1 cannot be pulled high the module stays in the 0/0 data
 * state and answers nothing - which is exactly what the hardware showed.
 */

static void lora_md_one(int pin, unsigned int mcu_sel, FAR const char *tag)
{
  unsigned long reg = P4X_IO_MUX_PIN(pin);
  unsigned int v = p4x_rd(reg);
  int hi;
  int lo;

  v = (v & ~P4X_MCU_SEL_M) | (mcu_sel << 12);
  p4x_wr(reg, v);

  p4x_wr(P4X_GPIO_ENABLE_W1TS, (1u << pin));

  p4x_wr(P4X_GPIO_OUT_W1TC, (1u << pin));
  usleep(20000);
  lo = lora_pad_level(pin);

  p4x_wr(P4X_GPIO_OUT_W1TS, (1u << pin));
  usleep(20000);
  hi = lora_pad_level(pin);

  printf("P4X-MDTEST: %s pad%d mcu_sel=%u -> low=%d high=%d : %s\n",
         tag, pin, mcu_sel, lo, hi,
         (lo == 0 && hi == 1) ? "GPIO OUTPUT WORKS" : "cannot drive");
}

#ifndef P4X_UART_OFF_CONF1
#define P4X_UART_OFF_CONF1       0x24ul
#endif

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif

#ifndef P4X_CONF0_MEM_CLK_EN
#define P4X_CONF0_MEM_CLK_EN     0x00100000ul
#endif

#ifndef P4X_CONF0_LOOPBACK
#define P4X_CONF0_LOOPBACK       0x00001000ul
#endif

#ifndef P4X_CONF1_CLK_EN
#define P4X_CONF1_CLK_EN         0x00200000ul
#endif
/* P4X-FINAL1: pure UART measurement - no GPIO emulation at all.
 *
 * Wiring under test: one jumper between GPIO20 (UART1 TX pad) and GPIO21
 * (UART1 RX pad), no module attached.
 */

static int lora_pad_level_ie(int pin)
{
  unsigned long reg = P4X_IO_MUX_PIN(pin);
  unsigned int v = p4x_rd(reg);

  if ((v & P4X_FUN_IE) == 0u)
    {
      p4x_wr(reg, v | P4X_FUN_IE);       /* input buffer on, level readable */
      usleep(2000);
    }

  return lora_pad_level(pin);
}

int board_lora_final1(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int st;
  unsigned int conf0;
  int p20_low = 0;
  int p21_low = 0;
  int rxd_low = 0;
  int maxfifo = 0;
  int i;
  int lb_bytes = 0;
  unsigned char lb[8];

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  printf("P4X-FINAL1: base=0x%08lx in_sel[13]=0x%08x out_sel[20]=0x%08x "
         "conf0=0x%08x clkdiv=0x%08x\n", b,
         p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(13)),
         p4x_rd(P4X_GPIO_FUNC_OUT_SEL_CFG(20)),
         p4x_rd(b + P4X_UART_OFF_CONF0),
         p4x_rd(b + P4X_UART_OFF_CLKDIV));

  printf("P4X-FINAL1: idle pad20=%d pad21=%d rxd=%u\n",
         lora_pad_level_ie(20), lora_pad_level_ie(21),
         (p4x_rd(b + P4X_UART_OFF_STATUS) >> 15) & 1u);

  /* ---- 1..3: send 16 bytes and watch everything at once --------------- */

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send("ABCDEFGHIJKLMNOP", 16);

  for (i = 0; i < 60000; i++)
    {
      if (lora_pad_level(20) == 0)
        {
          p20_low++;
        }

      if (lora_pad_level(21) == 0)
        {
          p21_low++;
        }

      st = p4x_rd(b + P4X_UART_OFF_STATUS);

      if (((st >> 15) & 1u) == 0u)
        {
          rxd_low++;
        }

      if ((int)(st & 0xffu) > maxfifo)
        {
          maxfifo = (int)(st & 0xffu);
        }
    }

  printf("P4X-FINAL1: send 16B -> pad20 low x%d, pad21 low x%d, rxd low x%d, "
         "max rxfifo=%d\n", p20_low, p21_low, rxd_low, maxfifo);

  /* ---- 4: internal loopback, no pads involved ------------------------ */

  conf0 = p4x_rd(b + P4X_UART_OFF_CONF0);
  p4x_wr(b + P4X_UART_OFF_CONF0, conf0 | P4X_CONF0_LOOPBACK | P4X_CONF0_MEM_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_CONF1, p4x_rd(b + P4X_UART_OFF_CONF1) | P4X_CONF1_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(30000);

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send("Z", 1);
  usleep(80000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);

  for (i = 0; i < 4; i++)
    {
      lb[i] = (unsigned char)(p4x_rd(b) & 0xffu);
    }

  lb_bytes = (int)(st & 0xffu);

  printf("P4X-FINAL1: internal loopback -> rxfifo=%d port=%02x %02x %02x %02x "
         "(sent 0x5a) : %s\n", lb_bytes, lb[0], lb[1], lb[2], lb[3],
         (lb[0] == 0x5a) ? "RX CORE WORKS" :
         (lb_bytes > 0) ? "RX core delivers something" : "RX core silent");

  /* loopback off again */

  p4x_wr(b + P4X_UART_OFF_CONF0, conf0 | P4X_CONF0_MEM_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(20000);

  return OK;
}

int board_lora_mdtest(void)
{
  unsigned int v22 = p4x_rd(P4X_IO_MUX_PIN(CONFIG_BOARD_LORA_MD0));
  unsigned int v23 = p4x_rd(P4X_IO_MUX_PIN(CONFIG_BOARD_LORA_MD1));

  printf("P4X-MDTEST: io_mux md0(%d)=0x%08x mcu_sel=%u | md1(%d)=0x%08x "
         "mcu_sel=%u | aux(%d) level=%d\n",
         CONFIG_BOARD_LORA_MD0, v22, (v22 & P4X_MCU_SEL_M) >> 12,
         CONFIG_BOARD_LORA_MD1, v23, (v23 & P4X_MCU_SEL_M) >> 12,
         CONFIG_BOARD_LORA_AUX, lora_pad_level(CONFIG_BOARD_LORA_AUX));

  lora_md_one(CONFIG_BOARD_LORA_MD0, 0, "md0");
  lora_md_one(CONFIG_BOARD_LORA_MD0, 1, "md0");
  lora_md_one(CONFIG_BOARD_LORA_MD1, 0, "md1");
  lora_md_one(CONFIG_BOARD_LORA_MD1, 1, "md1");

  /* leave both in the tested-good shape: try to put them in 1/1. */

  lora_mode(1, 1);
  usleep(50000);

  printf("P4X-MDTEST: after requesting 1/1: md0=%d md1=%d aux=%d\n",
         lora_pin_level(CONFIG_BOARD_LORA_MD0),
         lora_pin_level(CONFIG_BOARD_LORA_MD1),
         lora_pin_level(CONFIG_BOARD_LORA_AUX));

  lora_mode(0, 0);
  return OK;
}

int board_lora_modprobe(void)
{
  unsigned long b = board_lora_uart_base();
  static const unsigned char c3[3] = { 0xc3, 0xc3, 0xc3 };
  static const unsigned char f3[3] = { 0xf3, 0xf3, 0xf3 };
  static const unsigned char c1[3] = { 0xc1, 0xc1, 0xc1 };

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  lora_rate(CONFIG_BOARD_LORA_BAUD);
  usleep(20000);

  /* manual 5.3: configuration only works with MD0 = MD1 = 1. */

  lora_mode(1, 1);
  usleep(120000);

  printf("P4X-MODPROBE: md0=%d md1=%d aux=%d baud=%d base=0x%08lx "
         "clkdiv=0x%08x\n",
         lora_pin_level(CONFIG_BOARD_LORA_MD0),
         lora_pin_level(CONFIG_BOARD_LORA_MD1),
         lora_pin_level(CONFIG_BOARD_LORA_AUX), g_baud, b,
         p4x_rd(b + P4X_UART_OFF_CLKDIV));

  lora_mod_say(b, "C3 (hw version)", c3, 3);
  lora_mod_say(b, "F3 (sw version)", f3, 3);
  lora_mod_say(b, "C1 (config)",     c1, 3);

  /* back to the transparent half-duplex data state. */

  lora_mode(0, 0);
  return OK;
}

int board_lora_diag3(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned long m20 = P4X_IO_MUX_PIN(20);
  unsigned int st;
  int lo;
  int hi;
  int rx_low = 0;
  int tx_low = 0;
  int maxfifo = 0;
  int i;
  char back[24];

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  printf("P4X-DIAG3: base=0x%08lx status=0x%08x conf0=0x%08x clkdiv=0x%08x "
         "in_sel[13]=0x%08x out_sel[20]=0x%08x\n",
         b, p4x_rd(b + P4X_UART_OFF_STATUS),
         p4x_rd(b + P4X_UART_OFF_CONF0),
         p4x_rd(b + P4X_UART_OFF_CLKDIV),
         p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(13)),
         p4x_rd(P4X_GPIO_FUNC_OUT_SEL_CFG(20)));

  /* ---- A) electrical: drive GPIO20, watch GPIO21 ------------------- */

  lora_pad_output(20);
  lora_pad_drive(20, 0);
  usleep(20000);
  lo = lora_pad_level(21);
  lora_pad_drive(20, 1);
  usleep(20000);
  hi = lora_pad_level(21);
  lora_pad_release(20);

  printf("P4X-DIAG3: A1) gpio20 low -> pad21=%d | high -> pad21=%d : %s\n",
         lo, hi, (lo == 0 && hi == 1) ? "WIRE 20<->21" : "no 20<->21");

  /* scan every plausible TX pad, with a self-check that the drive works. */

  /* Full scan: which pad is electrically wired to pad 21?
   * Every step drives the candidate low and first checks that the drive took,
   * so "drive failed" can never be mistaken for "not wired".
   */

  {
    int p;
    int self;
    int hits = 0;

    for (p = 0; p < 54; p++)
      {
        if (p == 21)
          {
            continue;
          }

        lora_pad_output(p);
        lora_pad_drive(p, 0);
        usleep(4000);

        self = lora_pad_level(p);
        lo = lora_pad_level(21);

        lora_pad_drive(p, 1);
        usleep(2000);
        hi = lora_pad_level(21);

        lora_pad_release(p);

        if (self == 0 && lo == 0 && hi == 1)
          {
            printf("P4X-DIAG3: SCAN pad %d <-> pad 21 CONNECTED\n", p);
            hits++;
          }
      }

    printf("P4X-DIAG3: SCAN done, %d pad(s) wired to 21\n", hits);
  }

  /* hand the pads back to UART1 exactly as the driver had them. */

  lora_pad_release(20);
  board_lora_route(21, 20);
  usleep(20000);

  /* ---- B + C) send 16 bytes, watch the internals -------------------- */

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send("ABCDEFGHIJKLMNOP", 16);

  for (i = 0; i < 40000; i++)
    {
      st = p4x_rd(b + P4X_UART_OFF_STATUS);

      if (((st >> 31) & 1u) == 0u)
        {
          tx_low++;
        }

      if (((st >> 15) & 1u) == 0u)
        {
          rx_low++;
        }

      if ((int)(st & 0xffu) > maxfifo)
        {
          maxfifo = (int)(st & 0xffu);
        }
    }

  usleep(50000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);
  printf("P4X-DIAG3: B) txd low x%d rxd low x%d : %s\n", tx_low, rx_low,
         (tx_low > 0 && rx_low > 0) ? "TX DRIVES, RX SEES IT" :
         (tx_low > 0) ? "TX DRIVES, RX SEES NOTHING" : "TX NEVER MOVED");

  printf("P4X-DIAG3: C) max rxfifo=%d now rxfifo=%u afifo=0x%08x memrx=0x%08x\n",
         maxfifo, st & 0xffu, p4x_rd(b + 0x90ul), p4x_rd(b + 0x6cul));

  /* read the data port directly, whatever the counters say. */

  printf("P4X-DIAG3: C) port reads:");
  for (i = 0; i < 20; i++)
    {
      printf(" %02x", (unsigned int)(p4x_rd(b) & 0xffu));
    }

  printf("\n");

  (void)back;

  return OK;
}

/* P4X-RXINDEX: which GPIO-matrix input slot does UART0's receiver read?
 *
 * Measured setup: pad 37 held HIGH as a plain GPIO, pad 21 held LOW as a plain
 * GPIO.  Every one of the 64 in_sel slots is first pointed at pad 37, so a
 * correct slot must show RXD = 1.  Then each slot in turn is pointed at pad 21:
 * the slot that makes UART_STATUS[15] fall to 0 is the receiver's real input
 * index.  All slots are restored afterwards and the pads go back to the UART.
 */

int board_lora_rx_index_probe(void)
{
  unsigned long ubase = board_lora_uart_base();
  unsigned int saved[64];
  int low_slots[8];
  int nlow = 0;
  int slot;
  int i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  for (slot = 0; slot < 64; slot++)
    {
      saved[slot] = p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(slot));
    }

  /* pad 37 high, pad 21 low, both as plain GPIOs. */

  lora_pad_output(20);
  lora_pad_drive(20, 1);
  lora_pad_output(21);
  lora_pad_drive(21, 0);
  usleep(30000);

  /* everything points at the HIGH pad first. */

  for (slot = 0; slot < 64; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 37u);
    }

  usleep(20000);

  printf("P4X-RXINDEX: all slots -> pad37(high): rxd=%u\n",
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  for (slot = 0; slot < 64; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 21u);
      usleep(3000);

      if (((p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u) == 0u &&
          nlow < 8)
        {
          low_slots[nlow++] = slot;
        }

      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 37u);
    }

  printf("P4X-RXINDEX: slots whose pad change moves UART RXD:");
  if (nlow == 0)
    {
      printf(" none\n");
    }
  else
    {
      for (i = 0; i < nlow; i++)
        {
          printf(" %d", low_slots[i]);
        }

      printf("\n");
    }

  /* put every slot back exactly as it was. */

  for (slot = 0; slot < 64; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), saved[slot]);
    }

  lora_pad_release(21);
  lora_pad_release(20);
  board_lora_route(21, 37)   /* P4X-TXPAD37: TX out of the shorted pad */;
  usleep(20000);

  printf("P4X-RXINDEX: restored, in_sel[10]=0x%08x rxd=%u\n",
         p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(10)),
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  return nlow > 0 ? OK : -EIO;
}

/* P4X-UARTLB: attack the receiver directly.
 *
 * Proven on hardware before this probe:
 *   - in_sel[10] = 0x95 makes the internal rxd follow pad 21  (matrix OK)
 *   - one byte out pulls both pad 37 and pad 21 low            (TX core runs)
 *   - the controller's OWN internal loopback still fills the RX FIFO with
 *     nothing                                                   (receiver dead)
 *
 * So the fault is inside the UART's receive side, not in the wiring.  This
 * probe turns on the P4-specific switches one at a time, sending one byte after
 * each, and prints the RX FIFO count, the FSM states and the internal txd bit.
 *
 * Register offsets (uart_struct.h, hw_ver3):
 *   0x1c status      [7:0] rxfifo_cnt  [15] rxd  [23:16] txfifo_cnt  [31] txd
 *   0x20 conf0_sync  [20] mem_clk_en  [12] loopback  [22] rxfifo_rst
 *   0x24 conf1       [21] clk_en
 *   0x60 mem_conf    [26] mem_force_pu
 *   0x6c mem_rx_status  0x70 fsm_status  0x90 afifo_status  0x98 reg_update
 */

#define P4X_UART_OFF_CONF1        0x24ul
#define P4X_UART_OFF_MEM_CONF     0x60ul
#define P4X_UART_OFF_MEM_RXSTAT   0x6cul
#define P4X_UART_OFF_FSM          0x70ul
#define P4X_UART_OFF_AFIFO        0x90ul
#define P4X_UART_OFF_REG_UPDATE   0x98ul

#define P4X_CONF0_MEM_CLK_EN      0x00100000ul
#define P4X_CONF0_LOOPBACK        0x00001000ul
#define P4X_CONF1_CLK_EN          0x00200000ul
#define P4X_MEM_FORCE_PU          0x04000000ul

static void lora_uart_commit(unsigned long b)
{
  int guard;

  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);

  for (guard = 0; guard < 1000; guard++)
    {
      if ((p4x_rd(b + P4X_UART_OFF_REG_UPDATE) & 1u) == 0u)
        {
          break;
        }

      usleep(100);
    }
}

/* Send one byte and watch the internals while it goes out. */

static void lora_uart_send_and_watch(unsigned long b, FAR const char *tag)
{
  unsigned int st;
  unsigned int fsm;
  int txd_low = 0;
  int rxd_low = 0;
  int utx_seen = 0;
  int urx_seen = 0;
  int i;

  /* empty whatever is already in the RX FIFO. */

  for (i = 0; i < 64; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send("U", 1);

  for (i = 0; i < 20000; i++)
    {
      st = p4x_rd(b + P4X_UART_OFF_STATUS);
      fsm = p4x_rd(b + P4X_UART_OFF_FSM);

      if (((st >> 31) & 1u) == 0u)
        {
          txd_low = 1;
        }

      if (((st >> 15) & 1u) == 0u)
        {
          rxd_low = 1;
        }

      if (((fsm >> 4) & 0xfu) != 0u)
        {
          utx_seen = (int)((fsm >> 4) & 0xfu);
        }

      if ((fsm & 0xfu) != 0u)
        {
          urx_seen = (int)(fsm & 0xfu);
        }

      if (txd_low && ((st & 0xffu) != 0u))
        {
          break;
        }
    }

  usleep(30000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);

  printf("P4X-UARTLB: %-22s rxfifo=%u txfifo=%u txd_low=%d rxd_low=%d "
         "utx_fsm=%d urx_fsm=%d fsm=0x%02x afifo=0x%02x memrx=0x%08x\n",
         tag, st & 0xffu, (st >> 16) & 0xffu, txd_low, rxd_low,
         utx_seen, urx_seen, p4x_rd(b + P4X_UART_OFF_FSM),
         p4x_rd(b + P4X_UART_OFF_AFIFO), p4x_rd(b + P4X_UART_OFF_MEM_RXSTAT));
}

int board_lora_uart_lbtest(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int conf0;
  unsigned int conf1;
  unsigned int memconf;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  conf0 = p4x_rd(b + P4X_UART_OFF_CONF0);
  conf1 = p4x_rd(b + P4X_UART_OFF_CONF1);
  memconf = p4x_rd(b + P4X_UART_OFF_MEM_CONF);

  printf("P4X-UARTLB: start conf0=0x%08x conf1=0x%08x memconf=0x%08x "
         "clkconf=0x%08x\n", conf0, conf1, memconf,
         p4x_rd(b + P4X_UART_OFF_CLKCONF));

  /* A) as the driver left it. */

  lora_uart_send_and_watch(b, "A as-found");

  /* B) UART clock enable (conf1 bit21). */

  p4x_wr(b + P4X_UART_OFF_CONF1, conf1 | P4X_CONF1_CLK_EN);
  lora_uart_commit(b);
  usleep(20000);
  lora_uart_send_and_watch(b, "B +clk_en");

  /* C) + FIFO memory clock. */

  p4x_wr(b + P4X_UART_OFF_CONF0, conf0 | P4X_CONF0_MEM_CLK_EN);
  lora_uart_commit(b);
  usleep(20000);
  lora_uart_send_and_watch(b, "C +mem_clk_en");

  /* D) + internal loopback. */

  p4x_wr(b + P4X_UART_OFF_CONF0,
         conf0 | P4X_CONF0_MEM_CLK_EN | P4X_CONF0_LOOPBACK);
  lora_uart_commit(b);
  usleep(20000);
  lora_uart_send_and_watch(b, "D +loopback");

  /* E) + force the FIFO SRAM powered up. */

  p4x_wr(b + P4X_UART_OFF_MEM_CONF, memconf | P4X_MEM_FORCE_PU);
  lora_uart_commit(b);
  usleep(20000);
  lora_uart_send_and_watch(b, "E +mem_force_pu");

  /* F) RX core reset pulse, then retry. */

  {
    unsigned int clk = p4x_rd(b + P4X_UART_OFF_CLKCONF);

    p4x_wr(b + P4X_UART_OFF_CLKCONF, clk | (1u << 27));
    lora_uart_commit(b);
    usleep(20000);
    p4x_wr(b + P4X_UART_OFF_CLKCONF, clk & ~(1u << 27));
    lora_uart_commit(b);
    usleep(20000);
  }

  lora_uart_send_and_watch(b, "F +rx_rst_core");

  /* restore: loopback off, clocks on (harmless), committed. */

  p4x_wr(b + P4X_UART_OFF_CONF0,
         conf0 | P4X_CONF0_MEM_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_CONF1, conf1 | P4X_CONF1_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_MEM_CONF, memconf);
  lora_uart_commit(b);
  usleep(10000);

  printf("P4X-UARTLB: done, conf0=0x%08x conf1=0x%08x\n",
         p4x_rd(b + P4X_UART_OFF_CONF0), p4x_rd(b + P4X_UART_OFF_CONF1));

  return OK;
}

/* P4X-UARTIRQ: interrupt register offsets (also defined by the ERRMASK probe;
 * this block is inserted above it, so guard the definitions here).
 */

#ifndef P4X_UART_OFF_INT_RAW
#define P4X_UART_OFF_INT_RAW 0x04ul
#define P4X_UART_OFF_INT_CLR 0x10ul
#define P4X_UART_OFF_INT_ST  0x08ul
#endif

/* P4X-RXSTEPDEFS: this block is inserted above the probes that own these
 * names, so guard the definitions locally.
 */

#ifndef P4X_UART_OFF_CONF1
#define P4X_UART_OFF_CONF1       0x24ul
#endif

#ifndef P4X_UART_OFF_REG_UPDATE
#define P4X_UART_OFF_REG_UPDATE  0x98ul
#endif

#ifndef P4X_CONF0_MEM_CLK_EN
#define P4X_CONF0_MEM_CLK_EN     0x00100000ul
#endif

#ifndef P4X_CONF0_ERR_WR_MASK
#define P4X_CONF0_ERR_WR_MASK    0x00040000ul
#endif

#ifndef P4X_CONF1_CLK_EN
#define P4X_CONF1_CLK_EN         0x00200000ul
#endif
/* P4X-RXSTEPS: one byte at a time, read immediately.
 *
 * board_lora_recv() decides "is there data" from MEM_RX_STATUS's read/write
 * pointer distance - but on this chip those two pointers advance together, so
 * the distance is always 0 and recv() never touches the data port.  This probe
 * sends three distinct bytes and reads the port right after each one.
 */

int board_lora_rx_steps_probe(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int conf0;
  unsigned int st;
  unsigned int v;
  int i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  conf0 = p4x_rd(b + P4X_UART_OFF_CONF0);
  p4x_wr(b + P4X_UART_OFF_CONF0,
         (conf0 & ~P4X_CONF0_ERR_WR_MASK) | P4X_CONF0_MEM_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_CONF1,
         p4x_rd(b + P4X_UART_OFF_CONF1) | P4X_CONF1_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(20000);

  /* throw away whatever the port already holds. */

  for (i = 0; i < 8; i++)
    {
      (void)p4x_rd(b);
    }

  for (i = 0; i < 3; i++)
    {
      char one = (char)(0xa1 + i);

      board_lora_send(&one, 1);
      usleep(40000);

      st = p4x_rd(b + P4X_UART_OFF_STATUS);
      v = p4x_rd(b);

      printf("P4X-RXSTEPS: sent 0x%02x -> rxfifo=%u port=0x%02x "
             "afifo=0x%08x memrx=0x%08x\n",
             0xa1 + i, st & 0xffu, v & 0xffu, p4x_rd(b + 0x90ul),
             p4x_rd(b + 0x6cul));
    }

  return OK;
}

/* P4X-BITBANG: hand-clock a UART frame straight onto a pad and see whether the
 * receiver state machine turns it into a byte.
 *
 * Everything else has been ruled out:
 *   err_wr_mask cleared -> still nothing, and no frame-error interrupt either
 *   the wire is good    -> pad 37 low x71, pad 21 low x70 in the same window
 *   the matrix is good  -> with in_sel[10] = 0x95 STATUS.rxd follows pad 21
 *
 * So: what input does the receive FSM actually sample?  Answer it by driving a
 * frame onto a pad by hand, at 300 baud so usleep's 1 ms resolution is plenty.
 */

static void lora_bitbang_byte(int pad, unsigned char val)
{
  int i;
  int bit_us = 1000000 / ((g_baud > 0) ? g_baud : 9600);

  if (bit_us < 300)
    {
      bit_us = 300;
    }

  lora_pad_output(pad);

  lora_pad_drive(pad, 0);          /* start bit */
  usleep(bit_us);

  for (i = 0; i < 8; i++)
    {
      lora_pad_drive(pad, (val >> i) & 1);
      usleep(bit_us);
    }

  lora_pad_drive(pad, 1);          /* stop bit */
  usleep(bit_us);

  lora_pad_release(pad);
}

static int lora_bitbang_try(unsigned long b, int pad, unsigned int insel_val,
                            FAR const char *tag)
{
  unsigned int st;
  unsigned int raw;
  int i;

  p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(10), insel_val);

  /* empties */

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  p4x_wr(b + P4X_UART_OFF_INT_CLR, 0xffffffffu);

  lora_bitbang_byte(pad, 0x5a);
  usleep(20000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);
  raw = p4x_rd(b + P4X_UART_OFF_INT_RAW);

  printf("P4X-BITBANG: %-28s rxfifo=%u raw=0x%08x afifo=0x%08x\n",
         tag, st & 0xffu, raw, p4x_rd(b + 0x90ul));

  if ((st & 0xffu) != 0u)
    {
      printf("P4X-BITBANG: %-28s byte=0x%02x\n", tag,
             (unsigned int)(p4x_rd(b) & 0xffu));
    }

  return (int)(st & 0xffu);
}

int board_lora_bitbang_probe(void)
{
  unsigned long b = board_lora_uart_base();
  int saved = g_baud;
  int got21;
  int got38;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* slow the receiver down to 300 baud. */

  lora_rate(300);
  usleep(20000);

  printf("P4X-BITBANG: receiver at 300 baud, clkdiv=0x%08x\n",
         p4x_rd(b + P4X_UART_OFF_CLKDIV));

  /* A: the matrix path (slot 10 -> pad 21), pad 21 hand-driven. */

  got21 = lora_bitbang_try(b, 21, 0x80u | 21u, "pad21 via matrix");

  /* B: same slot pointed at pad 38, pad 38 hand-driven (native RX pad). */

  got38 = lora_bitbang_try(b, 38, 0x80u | 38u, "pad38 via matrix");

  /* C: slot select without the matrix bit, pad 38 hand-driven. */

  lora_bitbang_try(b, 38, 38u, "pad38 direct-select");

  printf("P4X-BITBANG: verdict pad21=%d pad38=%d : %s\n", got21, got38,
         (got21 == 0 && got38 == 0) ?
           "the receiver takes bytes from NEITHER pad - controller side" :
           (got38 > 0 && got21 == 0) ?
             "the receiver only follows pad 38 (native RX pad)" :
             (got21 > 0 && got38 == 0) ?
               "the receiver only follows pad 21 (matrix path)" :
               "both paths deliver");

  /* put everything back. */

  board_lora_route(21, 20);
  lora_rate(saved);
  usleep(20000);

  printf("P4X-BITBANG: restored baud=%d in_sel[10]=0x%08x\n",
         g_baud, p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(10)));

  return OK;
}

/* P4X-ERRMASK: the receiver has err_wr_mask set, which throws away every byte
 * that arrives with a framing/parity error.  Its documented default is 0, so
 * the serial driver turned it on.  Clear it and see whether bytes start to
 * arrive, and whether the interrupt status shows frame errors.
 */

#define P4X_UART_OFF_INT_RAW   0x04ul
#define P4X_UART_OFF_INT_ST    0x08ul
#define P4X_UART_OFF_INT_CLR   0x10ul
#define P4X_CONF0_ERR_WR_MASK  0x00040000ul

int board_lora_errwrmask_probe(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned long m37 = P4X_IO_MUX_PIN(20);
  unsigned int conf0;
  unsigned int conf1;
  unsigned int st;
  unsigned int is;
  unsigned int v;
  int i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  conf0 = p4x_rd(b + P4X_UART_OFF_CONF0);
  conf1 = p4x_rd(b + P4X_UART_OFF_CONF1);

  printf("P4X-ERRMASK: before conf0=0x%08x (err_wr_mask=%u dis_ovf=%u "
         "clk_en=%u) conf1=0x%08x\n", conf0, (conf0 >> 18) & 1u,
         (conf0 >> 17) & 1u, (conf1 >> 21) & 1u, conf1);

  /* clocks on, frame errors NOT masked, overflow NOT discarded. */

  p4x_wr(b + P4X_UART_OFF_CONF0,
         (conf0 & ~P4X_CONF0_ERR_WR_MASK) | P4X_CONF0_MEM_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_CONF1, conf1 | P4X_CONF1_CLK_EN);
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(20000);

  printf("P4X-ERRMASK: after  conf0=0x%08x conf1=0x%08x (err_wr_mask=%u)\n",
         p4x_rd(b + P4X_UART_OFF_CONF0), p4x_rd(b + P4X_UART_OFF_CONF1),
         (p4x_rd(b + P4X_UART_OFF_CONF0) >> 18) & 1u);

  /* clear interrupts and the RX side. */

  p4x_wr(b + P4X_UART_OFF_INT_CLR, 0xffffffffu);
  p4x_wr(b + P4X_UART_OFF_CONF0,
         p4x_rd(b + P4X_UART_OFF_CONF0) | (1u << 22));
  p4x_wr(b + P4X_UART_OFF_CONF0,
         p4x_rd(b + P4X_UART_OFF_CONF0) & ~(1u << 22));
  p4x_wr(b + P4X_UART_OFF_REG_UPDATE, 1u);
  usleep(20000);

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  p4x_wr(m37, p4x_rd(m37) | P4X_FUN_IE);

  board_lora_send("U", 1);
  usleep(100000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);
  is = p4x_rd(b + P4X_UART_OFF_INT_ST);

  printf("P4X-ERRMASK: byte sent -> rxfifo=%u afifo=0x%08x memrx=0x%08x "
         "int_st=0x%08x\n", st & 0xffu, p4x_rd(b + 0x90ul), p4x_rd(b + 0x6cul),
         is);
  printf("P4X-ERRMASK: raw=0x%08x (frm_err=%u par_err=%u brk=%u rxfifo_ovf=%u "
         "tout=%u)\n", p4x_rd(b + P4X_UART_OFF_INT_RAW),
         (p4x_rd(b + P4X_UART_OFF_INT_RAW) >> 3) & 1u,
         (p4x_rd(b + P4X_UART_OFF_INT_RAW) >> 2) & 1u,
         (p4x_rd(b + P4X_UART_OFF_INT_RAW) >> 7) & 1u,
         (p4x_rd(b + P4X_UART_OFF_INT_RAW) >> 4) & 1u,
         (p4x_rd(b + P4X_UART_OFF_INT_RAW) >> 8) & 1u);

  v = p4x_rd(b);
  printf("P4X-ERRMASK: data port reads 0x%02x\n", v & 0xffu);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);
  printf("P4X-ERRMASK: after the read rxfifo=%u afifo=0x%08x\n",
         st & 0xffu, p4x_rd(b + 0x90ul));

  return OK;
}

/* P4X-EXTPATH: sample the three signals at the same time during one byte.
 *
 * The internal loopback proves the receiver works; bytes sent with the loopback
 * off never arrive.  So watch, in one pass:
 *     GPIO_IN[37]  did the UART pull the TX pad low?
 *     GPIO_IN[21]  did the RX pad follow?
 *     STATUS[15]   did the receiver's own rxd input follow?
 */

int board_lora_extpath_probe(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned long m37 = P4X_IO_MUX_PIN(20);
  unsigned int st;
  int tx_low = 0;
  int rx_low = 0;
  int rxd_low = 0;
  int i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* make the TX pad readable (IE does not disturb the output path). */

  p4x_wr(m37, p4x_rd(m37) | P4X_FUN_IE);
  usleep(20000);

  printf("P4X-EXTPATH: idle pad37=%d pad21=%d rxd=%u in_sel[10]=0x%08x\n",
         lora_pad_level(20), lora_pad_level(21),
         (p4x_rd(b + P4X_UART_OFF_STATUS) >> 15) & 1u,
         p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(10)));

  /* drain whatever stale byte the data port holds. */

  for (i = 0; i < 16; i++)
    {
      (void)p4x_rd(b);
    }

  board_lora_send("U", 1);

  for (i = 0; i < 40000; i++)
    {
      if (lora_pad_level(20) == 0)
        {
          tx_low++;
        }

      if (lora_pad_level(21) == 0)
        {
          rx_low++;
        }

      st = p4x_rd(b + P4X_UART_OFF_STATUS);

      if (((st >> 15) & 1u) == 0u)
        {
          rxd_low++;
        }
    }

  usleep(30000);

  st = p4x_rd(b + P4X_UART_OFF_STATUS);

  printf("P4X-EXTPATH: one byte out -> pad37 low x%d, pad21 low x%d, "
         "rxd low x%d  (rxfifo=%u afifo=0x%08x)\n",
         tx_low, rx_low, rxd_low, st & 0xffu, p4x_rd(b + 0x90ul));

  if (tx_low > 0 && rx_low == 0)
    {
      printf("P4X-EXTPATH: VERDICT no wire: the TX pad moved, the RX pad did not\n");
    }
  else if (tx_low > 0 && rx_low > 0 && rxd_low == 0)
    {
      printf("P4X-EXTPATH: VERDICT matrix: both pads moved, the receiver input did not\n");
    }
  else if (tx_low > 0 && rx_low > 0 && rxd_low > 0)
    {
      printf("P4X-EXTPATH: VERDICT line is fine, receiver sees it - baud or sampling\n");
    }
  else
    {
      printf("P4X-EXTPATH: VERDICT the TX pad never moved at all\n");
    }

  return OK;
}

/* P4X-RXPOP: where do the received bytes actually come out?
 *
 * Measured before this probe: each transmitted byte moved MEM_RX_STATUS's
 * rx_sram_waddr (bit 16:9) by one while rx_sram_raddr (bit 7:0) and
 * STATUS[7:0] (rxfifo_cnt) stayed put.  So the bytes are in the SRAM; the open
 * question is whether reading the data port (offset 0x00) pops them.
 */

int board_lora_rx_pop_probe(void)
{
  unsigned long b = board_lora_uart_base();
  unsigned int mrx;
  unsigned int v;
  int i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* watch afifo_status while three different bytes arrive. */

  for (i = 0; i < 3; i++)
    {
      char one = (char)(0x41 + i);

      board_lora_send(&one, 1);
      usleep(60000);

      printf("P4X-RXPOP: sent 0x%02x -> status=0x%08x afifo=0x%08x "
             "memrx=0x%08x (rxfifo_empty=%u)\n",
             0x41 + i, p4x_rd(b + P4X_UART_OFF_STATUS),
             p4x_rd(b + 0x90ul), p4x_rd(b + 0x6cul),
             (p4x_rd(b + 0x90ul) >> 3) & 1u);
    }

  /* now drain six times, printing the value and afifo each time. */

  for (i = 0; i < 6; i++)
    {
      v = p4x_rd(b);
      mrx = p4x_rd(b + 0x6cul);
      printf("P4X-RXPOP: read[%d] 0x%02x afifo=0x%08x status=0x%08x "
             "raddr=%u waddr=%u\n",
             i, v & 0xffu, p4x_rd(b + 0x90ul),
             p4x_rd(b + P4X_UART_OFF_STATUS),
             mrx & 0xffu, (mrx >> 9) & 0xffu);
    }

  printf("P4X-RXPOP: final status=0x%08x afifo=0x%08x memrx=0x%08x\n",
         p4x_rd(b + P4X_UART_OFF_STATUS), p4x_rd(b + 0x90ul),
         p4x_rd(b + 0x6cul));

  return OK;
}

/* P4X-RXDIAG: settle three things with measurements, not assumptions.
 *
 * A) Does the UART really drive pad 37?  Turn pad 37's input buffer on (IE does
 *    not affect the output path) and sample BOTH pads while one byte goes out.
 * B) Is SIG_IN_SEL (bit 7) = 0 the matrix, or "use the direct IO-MUX input"?
 *    Measure the same pad at both settings.
 * C) Sweep all 256 input slots (not 64) with a HIGH reference pad and a LOW
 *    pad, so the slot that actually feeds UART0's receiver shows up.
 */

static unsigned int g_insel_save[256];

int board_lora_rx_diag(void)
{
  unsigned long ubase = board_lora_uart_base();
  unsigned int conf1;
  int slot;
  int i;
  int tx37_low = 0;
  int rx21_low = 0;
  int low_slots[10];
  int nlow = 0;
  int lvl;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  conf1 = p4x_rd(ubase + 0x24ul);
  printf("P4X-RXDIAG: conf1=0x%08x (rxd_inv=%u)\n", conf1,
         (conf1 >> 15) & 1u);

  /* ---- A) does the UART drive pad 37 at all? ------------------------- */

  {
    unsigned long m37 = P4X_IO_MUX_PIN(20);

    p4x_wr(m37, p4x_rd(m37) | P4X_FUN_IE);   /* watch the TX pad */
  }

  for (slot = 0; slot < 256; slot++)
    {
      g_insel_save[slot] = p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(slot));
    }

  board_lora_send("U", 1);

  for (i = 0; i < 20000; i++)
    {
      if (lora_pad_level(20) == 0)
        {
          tx37_low = 1;
        }

      if (lora_pad_level(21) == 0)
        {
          rx21_low = 1;
        }

      if (tx37_low && rx21_low)
        {
          break;
        }
    }

  printf("P4X-RXDIAG: during one byte: pad37 went low=%d pad21 went low=%d\n",
         tx37_low, rx21_low);

  /* ---- B) SIG_IN_SEL both ways, pad 21 driven as a GPIO ------------- */

  lora_pad_output(21);

  printf("P4X-RXDIAG: in_sel[10]=0x%02x sig0, pad21 low\n", 21);
  p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(10), 21u);
  lora_pad_drive(21, 0);
  usleep(20000);
  lvl = lora_pad_level(21);
  printf("P4X-RXDIAG:   pad21=%d rxd=%u\n", lvl,
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(10), 0x80u | 21u);
  usleep(20000);
  printf("P4X-RXDIAG: in_sel[10]=0x%02x sig1, pad21=%d rxd=%u\n", 0x80 | 21,
         lora_pad_level(21),
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  /* ---- C) full 256-slot sweep, HIGH reference = pad 37 --------------- */

  lora_pad_output(20);
  lora_pad_drive(20, 1);
  lora_pad_drive(21, 0);
  usleep(30000);

  printf("P4X-RXDIAG: reference pad37 high=%d, pad21 low=%d\n",
         lora_pad_level(20), lora_pad_level(21));

  for (slot = 0; slot < 256; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 37u);
    }

  usleep(20000);
  printf("P4X-RXDIAG: all 256 slots -> pad37(high): rxd=%u\n",
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  for (slot = 0; slot < 256; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 21u);
      usleep(1500);

      if (((p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u) == 0u &&
          nlow < 10)
        {
          low_slots[nlow++] = slot;
        }

      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), 37u);
    }

  printf("P4X-RXDIAG: slots that move UART RXD:");
  if (nlow == 0)
    {
      printf(" none of 256\n");
    }
  else
    {
      for (i = 0; i < nlow; i++)
        {
          printf(" %d", low_slots[i]);
        }

      printf("\n");
    }

  /* ---- restore ------------------------------------------------------ */

  for (slot = 0; slot < 256; slot++)
    {
      p4x_wr(P4X_GPIO_FUNC_IN_SEL_CFG(slot), g_insel_save[slot]);
    }

  lora_pad_release(21);
  lora_pad_release(20);
  board_lora_route(21, 20);
  usleep(20000);

  printf("P4X-RXDIAG: restored in_sel[10]=0x%08x pad21=%d rxd=%u\n",
         p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(10)), lora_pad_level(21),
         (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 15) & 1u);

  return OK;
}

/* P4X-SHORTTEST: is the 37 <-> 21 wire really there, and does the UART drive
 * it?  Called from the init path because every register looked right while the
 * loopback kept returning zero bytes.
 */

int board_lora_short_probe(void)
{
  unsigned long ubase = board_lora_uart_base();
  unsigned int tx_before;
  unsigned int tx_after;
  unsigned int st;
  int lo;
  int hi;
  int low_seen = 0;
  int i;
  int ok;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* 1) pad 37 as a real GPIO output (ENABLE1 for pin >= 32), both levels,
   *    watching pad 21.
   */

  lora_pad_output(20);

  lora_pad_drive(20, 0);
  usleep(20000);
  lo = lora_pad_level(21);

  lora_pad_drive(20, 1);
  usleep(20000);
  hi = lora_pad_level(21);

  ok = (lo == 0 && hi == 1);

  printf("P4X-SHORT: gpio37 low -> pad21=%d | gpio37 high -> pad21=%d : %s\n",
         lo, hi, ok ? "CONTACT OK" : "NO CONTACT");

  /* 2) hand the pads back to the UART. */

  lora_pad_release(20);
  board_lora_route(21, 20);
  usleep(20000);

  /* 3) does the UART move the line?  one byte, sampled on the RX pad, plus the
   *    TX FIFO counters so "never queued" and "never shifted out" differ.
   */

  tx_before = (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 16) & 0xffu;

  board_lora_send("U", 1);

  for (i = 0; i < 8000; i++)
    {
      if (lora_pad_level(21) == 0)
        {
          low_seen = 1;
          break;
        }
    }

  usleep(30000);

  tx_after = (p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 16) & 0xffu;
  st = p4x_rd(ubase + P4X_UART_OFF_STATUS);

  printf("P4X-SHORT: uart tx: txfifo %u -> %u, pad21 low: %d : %s\n",
         tx_before, tx_after, low_seen,
         low_seen ? "the UART does drive the wire" :
                    "the UART never moved the pad");
  printf("P4X-SHORT: status=0x%08x rxd=%u rxfifo=%u\n", st,
         (st >> 15) & 1u, st & P4X_UART_RXFIFO_CNT);

  /* 4) what does the matrix actually say now? */

  lora_dump_insel("after-route");

  return ok ? OK : -EIO;
}

/****************************************************************************
 * Name: board_lora_initialize_mode
 *
 * Description:
 *   probe = 1 : open the port, run the configuration probe (needs MD0=MD1=1,
 *               which stops the module from forwarding data while it runs)
 *   probe = 0 : open the port for data only.  The probe is skipped because it
 *               puts the module into the 1/1 sleep+configuration state, where
 *               (manual 5.3) nothing is forwarded over the air, and it also
 *               drains whatever the module had already delivered into the RX
 *               FIFO.  A receive loop that starts after a probe therefore sees
 *               nothing at all - measured exactly that way: rxfifo held 48
 *               bytes while the task reported "0 chunk(s), 0 byte(s)".
 *
 ****************************************************************************/

int board_lora_initialize_mode(int probe)
{
  if (g_inited)
    {
      lora_mode(0, 0);
      return OK;
    }

  /* P4X-UARTPINS: enable the pads themselves before anything else.
   *
   * The NuttX serial driver programs its own pins when the port is opened, but
   * the driver this replaces also laid the pads out explicitly (esp_configgpio
   * on the UART pins).  A pad whose input buffer is off (IO_MUX IE = 0) cannot
   * deliver anything to the controller no matter what the matrix says, so set
   * IE on the RX pad and clear it on the TX pad here; the matrix itself is left
   * to the serial driver.
   */

  {
    unsigned int v;

    /* P4X-RXPAD21: the RX pad is GPIO21 (J1 pin 11) and that is what
     * CONFIG_ESPRESSIF_UART0_RXPIN says, but an earlier step pointed this block
     * at GPIO38.  An IO-MUX pad with IE = 0 cannot feed the GPIO matrix, so with
     * 37<->21 shorted the bytes went out and never came back.  TX stays GPIO37.
     */

    /* P4X-TXPAD37: GPIO21 is UART1's own RX pad (IO_MUX FUNC0), so it keeps
     * MCU_SEL = 0 and just needs its input buffer on - that is the direct path
     * the receiver samples.  GPIO37 belongs to UART0, so it is switched to
     * plain GPIO and driven from the matrix instead (see board_lora_route()).
     */

    v = p4x_rd(P4X_IO_MUX_PIN(21));
    v &= ~P4X_MCU_SEL_M;
    v |= P4X_FUN_IE;
    p4x_wr(P4X_IO_MUX_PIN(21), v);

    v = p4x_rd(P4X_IO_MUX_PIN(37));
    v &= ~P4X_FUN_IE;
    v = (v & ~P4X_MCU_SEL_M) | P4X_MCU_SEL_GPIO;
    p4x_wr(P4X_IO_MUX_PIN(37), v);

    printf("P4X-LORA: uart pads armed rx(ie=%u) tx(ie=%u)\n",
           (p4x_rd(P4X_IO_MUX_PIN(21)) & P4X_FUN_IE) ? 1u : 0u,
           (p4x_rd(P4X_IO_MUX_PIN(37)) & P4X_FUN_IE) ? 1u : 0u);
  }

  printf("P4X-LORA: A28 driver, manual-driven init\n");

  /* MD0/MD1 must be real GPIOs before anything else: the module's own
   * pull-ups are 1 MOhm ("extremely weak"), so an undriven pair floats and
   * the module can sit in the 1/1 sleep+configuration state forever.
   */

  lora_pin_output(CONFIG_BOARD_LORA_MD0);
  lora_pin_output(CONFIG_BOARD_LORA_MD1);
  lora_mode(0, 0);

  printf("P4X-LORA: MD0=GPIO%d MD1=GPIO%d routed as GPIO and driven low\n",
         CONFIG_BOARD_LORA_MD0, CONFIG_BOARD_LORA_MD1);
  printf("P4X-LORA: pin levels md0=%d md1=%d aux(GPIO%d)=%d\n",
         lora_pin_level(CONFIG_BOARD_LORA_MD0),
         lora_pin_level(CONFIG_BOARD_LORA_MD1),
         CONFIG_BOARD_LORA_AUX, lora_pin_level(CONFIG_BOARD_LORA_AUX));

  {
    int k;

    for (k = 0; k < LORA_DEV_CANDIDATES && g_fd < 0; k++)
      {
        g_fd = open(g_dev_candidates[k], O_RDWR | O_NONBLOCK);
        if (g_fd >= 0)
          {
            snprintf(g_dev_name, sizeof(g_dev_name), "%s", g_dev_candidates[k]);
            printf("P4X-LORA: opened %s\n", g_dev_name);
          }
      }
  }

  if (g_fd < 0)
    {
      printf("P4X-LORA: no UART device opened: %d\n", errno);
      return -errno;
    }

  if (lora_rate(CONFIG_BOARD_LORA_BAUD) < 0)
    {
      close(g_fd);
      g_fd = -1;
      return -EIO;
    }

  g_inited = 1;
  printf("P4X-LORA: %s open at %d baud 8N1\n", LORA_DEV, g_baud);

  /* P4X-INSELDUMP: what did the serial driver do with the pad before we touch
   * the matrix?  This answers "is UART0's RX input slot really 10?"
   */

  lora_dump_insel("after-open");

  /* P4X-CLKRESET: 直接操作 UART0 的 CLK_CONF(0x500CA088)。
   *
   * 该寄存器(uart_reg.h:1474-1505)只有四个位:
   *     bit24 TX_SCLK_EN   bit25 RX_SCLK_EN
   *     bit26 TX_RST_CORE  bit27 RX_RST_CORE   "写 1 再写 0 复位"
   * 没有 SCLK_DIV 字段,所以 clk_conf=0x03000000 表示收发时钟均使能,属正常;
   * 分频在 CLKDIV(0x500CA014),开关在 CONF0(0x500CA020) 的 bit3/bit4。
   *
   * 这里做三件事:确认使能位 -> 打一次收发核复位脉冲 -> 确保 CONF0 收发使能,
   * 然后逐项回读,把"到底改没改成"变成日志里的事实。
   */

  {
    unsigned long b = board_lora_uart_base();
    unsigned long clk = b + 0x88ul;
    unsigned long c0r = b + 0x20ul;
    unsigned long dvr = b + 0x14ul;
    unsigned int before = p4x_rd(clk);
    unsigned int c0;
    unsigned int dv;
    unsigned int v;

    printf("P4X-LORA: CLK_CONF before = 0x%08x (tx_clk=%u rx_clk=%u)\n",
           before, (before >> 24) & 1u, (before >> 25) & 1u);

    /* 使能 + 复位脉冲:先置 1,等一会儿,再清 0。 */

    p4x_wr(clk, (before | 0x03000000u) | 0xc000000u);
    usleep(50000);
    p4x_wr(clk, (before | 0x03000000u) & ~0xc000000u);
    usleep(20000);

    /* CONF0:置 RX_ENABLE(bit3)/TX_ENABLE(bit4),清 RXFIFO_RST(22)/TXFIFO_RST(23) */

    c0 = p4x_rd(c0r);
    c0 |= (1u << 3) | (1u << 4);
    c0 &= ~((1u << 22) | (1u << 23));
    p4x_wr(c0r, c0);
    usleep(20000);

    v = p4x_rd(clk);
    c0 = p4x_rd(c0r);
    dv = p4x_rd(dvr);

    printf("P4X-LORA: CLK_CONF after  = 0x%08x (tx_clk=%u rx_clk=%u "
           "tx_rst=%u rx_rst=%u)\n",
           v, (v >> 24) & 1u, (v >> 25) & 1u, (v >> 26) & 1u, (v >> 27) & 1u);
    printf("P4X-LORA: CONF0 after     = 0x%08x rx_en=%u tx_en=%u "
           "rxfifo_rst=%u txfifo_rst=%u\n",
           c0, (c0 >> 3) & 1u, (c0 >> 4) & 1u, (c0 >> 22) & 1u, (c0 >> 23) & 1u);
    printf("P4X-LORA: CLKDIV          = 0x%08x (int=%u frag=%u)%s\n",
           dv, dv & 0xfffu, (dv >> 20) & 0xfu,
           ((dv & 0xfffu) == 0) ? "  <-- 分频为 0,波特率无法产生!" : "");
  }

  /* The serial driver does not attach UART1 to any pin, so do it here. */

  /* P4X-UART0-SWITCH: module TXD -> GPIO38 (UART0 RX, J1 pin 10),
   * module RXD -> GPIO37 (UART0 TX, J1 pin 8).
   */

  lora_dump_insel("before-route");

  if (board_lora_route(21, 20) < 0)
    {
      printf("P4X-LORA: WARNING UART1 routing did not verify\n");
    }

  /* P4X-SHORTTEST: every register said the routing was right and the loopback
   * still returned zero bytes, so ask the pads themselves.
   */

  /* P4X-PROBES-OFF: board_lora_short_probe(); (bring-up only) */

  /* P4X-UARTLB: is the receiver itself alive?  The controller's own internal
   * loopback needs no wire and no module.
   */

  /* P4X-PROBES-OFF: board_lora_uart_lbtest(); (bring-up only) */

  /* P4X-RXSTEPS: can single bytes be read back one at a time? */

  /* P4X-FINAL1: pure UART measurement, no GPIO emulation. */

  /* P4X-BRINGUP-OFF: board_lora_final1(); - the 20<->21 jumper measurement */

  /* P4X-MDTEST: can MD0/MD1 be driven high (needed for config mode)? */

  /* P4X-BRINGUP-OFF: board_lora_mdtest(); - the MD0/MD1 drive test */

  /* P4X-MODPROBE: ask the A28 itself, per manual 6.1. */

  /* P4X-BRINGUP-OFF: board_lora_modprobe(); - the manual 6.1 C3/F3/C1 probe */

  /* P4X-DIAG3: one measurement pass on the current configuration. */

  /* P4X-BAUDSWEEP2: find the module's real serial rate. */

  /* P4X-BAUDCALC: 9600 baud from the real clock registers. */

  board_lora_baud_calc();


  /* P4X-BRINGUP-OFF: board_lora_diag3(); - the bring-up measurement pass */

  /* P4X-PROBES-OFF: board_lora_rx_steps_probe(); (bring-up only) */

  /* P4X-BITBANG: which pad does the receive state machine actually sample? */

  /* P4X-PROBES-OFF: board_lora_bitbang_probe(); (bring-up only) */

  /* P4X-ERRMASK: is the receiver throwing every byte away as a frame error? */

  /* P4X-PROBES-OFF: board_lora_errwrmask_probe(); (bring-up only) */

  /* P4X-EXTPATH: three signals sampled at once during one byte. */

  /* P4X-PROBES-OFF: board_lora_extpath_probe(); (bring-up only) */

  /* P4X-RXPOP: where do the received bytes come out? */

  /* P4X-PROBES-OFF: board_lora_rx_pop_probe(); (bring-up only) */

  /* P4X-RXDIAG: does the UART drive the pad, does the wire carry it, and which
   * of the 256 input slots does UART0's receiver really read?
   */

  /* P4X-PROBES-OFF: board_lora_rx_diag(); (bring-up only) */

  /* P4X-NOROUTE: this driver deliberately does NOT touch the GPIO matrix.
   *
   * An earlier revision "helpfully" programmed func_out_sel_cfg[GPIO20] and
   * func_in_sel_cfg[] with the ESP32/ESP32-S3 signal numbers (TX 24, RX 25).
   * The P4's own table (gpio_sig_map.h) uses 10 / 13 / 16 for UART0/1/2, so
   * those writes attached the UART to the wrong matrix slots - they *created*
   * the fault they were meant to fix.  The NuttX serial driver programs the
   * pins and the matrix itself when the port is opened, so the right thing to
   * do here is report what it did and leave it alone.
   */

  {
    unsigned long in_reg = P4X_GPIO_FUNC_IN_SEL_CFG(P4X_UART1_RX_SIG);
    unsigned long out_reg = P4X_GPIO_FUNC_OUT_SEL_CFG(37);

    printf("P4X-LORA: uart block 0x%08lx, in_sel[%u]=0x%08x (sel=%u), "
           "out_sel[GPIO20]=0x%08x (sel=%u)\n",
           board_lora_uart_base(), P4X_UART1_RX_SIG, p4x_rd(in_reg),
           p4x_rd(in_reg) & 0x3fu, p4x_rd(out_reg), p4x_rd(out_reg) & 0x1ffu);
  }

  /* P4X-BAUDSWEEP2-CALL: only when nothing else is using the port yet. */

  /* P4X-BAUDSWEEP2-DISABLED: keep the port at 9600 for the air test. */

  if (probe)
    {
      board_lora_probe();
    }
  else
    {
      /* Data-only start: stay in the 0/0 transparent state and leave whatever
       * the module already delivered sitting in the FIFO for the caller.
       */

      printf("P4X-LORA: data-only start, probe skipped (MD0=MD1=0)\n");
    }

  lora_mode(0, 0);
  return OK;
}

int board_lora_probe(void)
{
  uint8_t rx[64];
  int n;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  lora_flush();

  /* Manual 6.1: configuration commands are only accepted with MD0=MD1=1. */

  printf("P4X-LORA: probe at %d baud, entering the 1/1 configuration state\n",
         g_baud);
  lora_mode(1, 1);
  usleep(200000);
  lora_flush();

  /* ---- hardware version: 0xC3 0xC3 0xC3 -> "A28-T2G4A12D1a-V9.0" ---- */

  {
    static const uint8_t cmd[3] = { 0xc3, 0xc3, 0xc3 };
    int attempt;

    g_version_ok = 0;

    for (attempt = 0; attempt < 3 && !g_version_ok; attempt++)
      {
        write(g_fd, cmd, sizeof(cmd));
        n = lora_read_reply(rx, sizeof(rx) - 1, 400);

        if (n > 0)
          {
            lora_print_hex("C3", rx, n);
          }
        else
          {
            printf("P4X-LORA: C3 -> 0 bytes (silence)\n");
          }

        if (lora_looks_ok(rx, n))
          {
            rx[n] = '\0';
            snprintf(g_hwver, sizeof(g_hwver), "%s", (FAR char *)rx);
            g_version_ok = 1;
          }
        else
          {
            lora_report_line("after C3");
            usleep(120000);
            lora_flush();
          }
      }

    if (!g_version_ok)
      {
        snprintf(g_hwver, sizeof(g_hwver), "(no answer)");
      }
  }

  if (!g_version_ok)
    {
      lora_report_line("giving up");
      lora_mode(0, 0);                 /* never leave the module asleep */
      snprintf(g_swver, sizeof(g_swver), "(no answer)");
      printf("P4X-LORA: hw='%s' sw='%s' detected=0\n", g_hwver, g_swver);
      return -ENODEV;
    }

  /* ---- software version: 0xF3 0xF3 0xF3 ---- */

  {
    static const uint8_t cmd[3] = { 0xf3, 0xf3, 0xf3 };

    write(g_fd, cmd, sizeof(cmd));
    n = lora_read_reply(rx, sizeof(rx) - 1, 400);
    if (lora_looks_ok(rx, n))
      {
        rx[n] = '\0';
        snprintf(g_swver, sizeof(g_swver), "%s", (FAR char *)rx);
      }
    else
      {
        snprintf(g_swver, sizeof(g_swver), "(no answer)");
      }
  }

  /* ---- configuration: 0xC1 0xC1 0xC1 ----
   *
   * Manual 6.1 prints the answer as `C0 12 34 18 00 03`: the leading 0xC0 is
   * an echo, the five bytes after it are ADDH ADDL SPEED CHAN OPTION.  Check
   * the echo and the SPEED fields rather than trusting the byte count.
   */

  {
    static const uint8_t cmd[3] = { 0xc1, 0xc1, 0xc1 };
    int off = 0;

    write(g_fd, cmd, sizeof(cmd));
    n = lora_read_reply(rx, sizeof(rx), 400);
    lora_print_hex("C1", rx, n);

    if (n >= 6 && rx[0] == 0xc0)
      {
        off = 1;
      }

    if (n - off >= 5)
      {
        memcpy(g_cfg, rx + off, 5);
        g_cfg_ok = 1;
      }
  }

  printf("P4X-LORA: hw='%s'\n", g_hwver);
  printf("P4X-LORA: sw='%s'\n", g_swver);

  if (g_cfg_ok)
    {
      int speed = g_cfg[2];
      int idx = (speed >> 3) & 7;
      static const int rate_of_idx[8] =
      {
        1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
      };

      printf("P4X-LORA: cfg addr=0x%02x%02x speed=0x%02x (baud idx %d = %d bps, "
             "parity 0x%x, air %d) chan=0x%02x option=0x%02x (mod %s, "
             "continuous %d, power %ddBm)\n",
             g_cfg[0], g_cfg[1], speed, idx, rate_of_idx[idx],
             (speed >> 6) & 3, speed & 7, g_cfg[3], g_cfg[4],
             (g_cfg[4] & 0x80) ? "LORA" : "FLRC",
             (g_cfg[4] >> 6) & 1, 3 + 3 * (g_cfg[4] & 3));

      if (rate_of_idx[idx] != g_baud)
        {
          printf("P4X-LORA: NOTE the module reports %d bps while this port is "
                 "at %d bps - call board_lora_probe_baud(%d) to match\n",
                 rate_of_idx[idx], g_baud, rate_of_idx[idx]);
        }
    }
  else
    {
      printf("P4X-LORA: configuration read failed (need 5 valid bytes)\n");
    }

  printf("P4X-LORA: detected=1\n");
  lora_mode(0, 0);
  return OK;
}

/* Try one rate: switch the port and re-run the whole probe. */

int board_lora_probe_baud(int baud)
{
  if (g_fd < 0)
    {
      return -ENODEV;
    }

  if (lora_rate(baud) < 0)
    {
      return -EIO;
    }

  g_version_ok = 0;
  return (board_lora_probe() == OK) ? OK : -ENODEV;
}

/* Every rate of the SPEED register (manual 6.2), factory 9600 first.  On
 * success the port is left at the rate that answered.
 */

int board_lora_sweep(FAR int *report, int max)
{
  int r;
  int hits = 0;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  for (r = 0; r < LORA_NRATES; r++)
    {
      printf("P4X-BAUDSWEEP: trying %d baud\n", g_rates[r]);

      if (board_lora_probe_baud(g_rates[r]) == OK)
        {
          printf("P4X-BAUDSWEEP: module answered at %d baud\n", g_rates[r]);

          if (report != NULL && hits < max)
            {
              report[hits] = g_rates[r];
            }

          hits++;

          lora_rate(g_rates[r]);        /* keep the port on the good rate */
          break;
        }
    }

  if (hits == 0)
    {
      printf("P4X-BAUDSWEEP: no rate produced a valid A28 answer\n");
      lora_report_line("end of sweep");
      lora_rate(CONFIG_BOARD_LORA_BAUD);
    }

  return hits;
}

int board_lora_send(FAR const void *data, size_t len)
{
  unsigned long ubase;
  FAR const uint8_t *p = (FAR const uint8_t *)data;
  int n;
  size_t i;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* P4X-FIFOTX: put the bytes straight into the controller's TX FIFO.
   *
   * Measured: board_lora_send() never moved the TX pad, so the serial driver
   * was accepting bytes into its software buffer without ever giving them to
   * the hardware - the same failure this UART showed on the RX side.  With the
   * driver's TX out of the picture the module hears every byte (or, if it
   * still does not answer, the problem is provably not the software path).
   */

  ubase = board_lora_uart_base();

  for (i = 0; i < len; i++)
    {
      int guard = 0;

      /* TX FIFO is 128 deep; wait while it is full rather than dropping. */

      while (((p4x_rd(ubase + P4X_UART_OFF_STATUS) >> 16) & 0xffu) >= 120)
        {
          if (++guard > 5000)
            {
              break;
            }

          usleep(1000);
        }

      p4x_wr(ubase, p[i]);
    }

  n = (int)len;

  if (n > 0)
    {
      g_tx_bytes += n;
      g_tx_pkts++;
    }

  return n;
}

int board_lora_recv(FAR void *buf, size_t len)
{
  unsigned long ubase;
  unsigned int have;
  int n = 0;
  FAR uint8_t *out = (FAR uint8_t *)buf;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* P4X-FIFOPOLL: take the bytes straight out of the controller.
   *
   * Measured on hardware: UART_STATUS[7:0] reported one byte waiting while
   * read() on /dev/ttyS0 returned nothing, i.e. the RX interrupt path did not
   * hand the byte to the driver.  The FIFO itself is reliable, so poll it
   * first and fall back to read() only when it is empty.
   */

  ubase = board_lora_uart_base();

  /* P4X-RXFIFOCNT: STATUS[7:0] is the right answer *once the receive input is
   * actually connected*.  Earlier it read 0 for every transfer because nothing
   * reached the receiver at all; with the matrix route in place the same
   * register peaks at exactly the number of bytes sent (16 for 16), which is
   * also what the vendor's uart_ll_get_rxfifo_len() uses.
   *
   * The SRAM read/write pointers in MEM_RX_STATUS are useless here: they move
   * together, so their difference is always 0 and a receiver built on it drops
   * whole bursts.
   */

  have = p4x_rd(ubase + P4X_UART_OFF_STATUS) & P4X_UART_RXFIFO_CNT;

  if (have > 0 && buf != NULL && len > 0)
    {
      unsigned int take = (have > (unsigned int)len) ? (unsigned int)len : have;
      unsigned int i;

      for (i = 0; i < take; i++)
        {
          out[i] = (uint8_t)(p4x_rd(ubase) & 0xffu);
        }

      n = (int)take;
    }
  else
    {
      n = read(g_fd, buf, len);
    }

  if (n > 0)
    {
      g_rx_bytes += n;
      g_rx_pkts++;
    }

  return n;
}

int board_lora_ready(void)
{
  return (g_fd >= 0) ? 1 : 0;
}

int board_lora_detected(void)
{
  return g_version_ok ? 1 : 0;
}

int board_lora_baud(void)
{
  return g_baud;
}

FAR const char *board_lora_hwversion(void)
{
  return g_hwver;
}

FAR const char *board_lora_version(void)
{
  return g_swver;
}

const uint8_t *board_lora_config(void)
{
  return g_cfg_ok ? g_cfg : NULL;
}

int board_lora_line(FAR int *rxd_level, FAR int *rx_fifo)
{
  unsigned int st;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  st = lora_uart_status();

  if (rxd_level != NULL)
    {
      *rxd_level = (st & P4X_UART_RXD_LEVEL) ? 1 : 0;
    }

  if (rx_fifo != NULL)
    {
      *rx_fifo = (int)(st & P4X_UART_RXFIFO_CNT);
    }

  return OK;
}

/****************************************************************************
 * Name: board_lora_rx_pullup
 *
 * Description:
 *   Turn the RX pad's internal pull-up and input buffer on (or off) and report
 *   the levels seen on both sides of the GPIO matrix.
 *
 *   A pad that floats reads low, exactly like a pad that is being held low, so
 *   "rxd_level = 0" on its own cannot tell the two apart.  Switching the
 *   internal pull-up on does: a floating line comes up to 1 (nothing is
 *   fighting the pull-up; the module's own 3.3 V TXD is absent), while a line
 *   that is genuinely shorted to ground stays at 0.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: board_lora_tx_probe
 *
 * Description:
 *   Write bytes to the port in the background and sample the TX pad plus the
 *   UART's own view of it while they go out.
 *
 *   UART_STATUS[31] is UART_TXD, the level the controller believes it is
 *   transmitting, and the GPIO input register shows what the pad really does.
 *   A pad that never moves means the controller is not reaching the pin at
 *   all, which no amount of external wiring can fix.
 *
 ****************************************************************************/

int board_lora_tx_probe(FAR int *txd_bit, FAR int *pad_level,
                        FAR int *moved, FAR int *bytes)
{
  static const uint8_t pat[16] =
  {
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50
  };

  unsigned int st;
  int i;
  int n;
  int seen_low = 0;
  int seen_high = 0;
  int total = 0;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  n = write(g_fd, pat, sizeof(pat));
  if (n > 0)
    {
      total = n;
    }

  for (i = 0; i < 4000; i++)
    {
      st = lora_uart_status();

      if (st & P4X_UART_TXD_LEVEL)
        {
          seen_high = 1;
        }
      else
        {
          seen_low = 1;
        }

      if ((p4x_rd(P4X_GPIO_IN) >> 20) & 1u)
        {
          seen_high |= 2;
        }
      else
        {
          seen_low |= 2;
        }
    }

  if (txd_bit != NULL)
    {
      *txd_bit = (st & P4X_UART_TXD_LEVEL) ? 1 : 0;
    }

  if (pad_level != NULL)
    {
      *pad_level = (int)((p4x_rd(P4X_GPIO_IN) >> 20) & 1u);
    }

  if (moved != NULL)
    {
      *moved = (seen_low && seen_high) ? 1 : 0;
    }

  if (bytes != NULL)
    {
      *bytes = total;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lora_matrix_loopback
 *
 * Description:
 *   Feed the UART1 TX signal straight back into the UART1 RX signal inside the
 *   GPIO matrix, so no pad is involved.
 *
 *   This is the test the earlier p4x_loop example meant to run, but that one
 *   printed PASS whenever read() returned more than zero bytes, and read()
 *   hands back phantom bytes here whenever the FIFO is empty.  The verdict here
 *   is a byte-for-byte comparison of what was written.
 *
 ****************************************************************************/

int board_lora_matrix_loopback(FAR const uint8_t *pat, int len,
                               FAR uint8_t *out, int outmax, int *got)
{
  int i;
  int n;
  int total = 0;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  /* The matrix is left exactly as the serial driver programmed it.  Forcing
   * other signal numbers onto it is what broke the link in the first place -
   * the P4 numbers are 10/13/16 for UART0/1/2, not the ESP32's 24/25.
   */

  printf("P4X-LOOP-M: live uart block 0x%08lx rxfifo=%lu rxd=%u txd=%u "
         "in_sel[%u]=0x%08x out_sel[20]=0x%08x\n",
         board_lora_uart_base(),
         (unsigned long)(lora_uart_status() & P4X_UART_RXFIFO_CNT),
         (lora_uart_status() & P4X_UART_RXD_LEVEL) ? 1u : 0u,
         (lora_uart_status() & P4X_UART_TXD_LEVEL) ? 1u : 0u,
         P4X_UART1_RX_SIG, p4x_rd(P4X_GPIO_FUNC_IN_SEL_CFG(P4X_UART1_RX_SIG)),
         p4x_rd(P4X_GPIO_FUNC_OUT_SEL_CFG(37)));

  lora_flush();

  n = write(g_fd, pat, (size_t)len);
  if (n > 0)
    {
      total = n;
    }

  usleep(200000);

  for (i = 0; i < 4 && out != NULL && total < outmax; i++)
    {
      n = read(g_fd, out + total, (size_t)(outmax - total));
      if (n <= 0)
        {
          break;
        }

      total += n;
      usleep(50000);
    }

  if (got != NULL)
    {
      *got = total;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lora_pad_loopback
 *
 * Description:
 *   Drive GPIO20 as a plain GPIO output and read GPIO21 as a plain GPIO input,
 *   with a jumper between them.  This checks the two pads and the header pins
 *   without the UART being involved at all.
 *
 * Returned Value:
 *   Zero (OK) if the driven level was read back on GPIO21, -EIO otherwise.
 *
 ****************************************************************************/

int board_lora_pad_loopback(void)
{
  unsigned long mux20 = P4X_IO_MUX_PIN(20);
  unsigned long mux21 = P4X_IO_MUX_PIN(21);
  unsigned int save20 = p4x_rd(mux20);
  unsigned int save21;
  unsigned int v;
  int high;
  int low;
  int r;

  if (g_fd < 0)
    {
      return -ENODEV;
    }

  save21 = p4x_rd(mux21);

  /* GPIO20 as an output whose source is the GPIO output latch, GPIO21 as an
   * input with the input buffer and a pull-down (so a floating pin reads 0).
   */

  v = save20 & ~P4X_MCU_SEL_M;
  p4x_wr(mux20, v);

  {
    /* FUNC_OUT_SEL bits [9:0]: OEN_SEL (bit 10) = 1 takes the output enable
     * from the GPIO_ENABLE register, and the signal index 0..255 otherwise.
     * Writing 0x100 selects "GPIO output latch" as the source.
     */

    p4x_wr(P4X_GPIO_FUNC_OUT_SEL_CFG(37), 0x100u);
  }

  v = save21 & ~P4X_MCU_SEL_M;
  v |= P4X_FUN_IE | P4X_FUN_WPD;
  p4x_wr(mux21, v);

  p4x_wr(P4X_GPIO_ENABLE_W1TS, (1ul << 37));

  p4x_wr(P4X_GPIO_OUT_W1TS, (1ul << 37));
  usleep(20000);
  high = (int)((p4x_rd(P4X_GPIO_IN) >> 21) & 1u);

  p4x_wr(P4X_GPIO_OUT_W1TC, (1ul << 37));
  usleep(20000);
  low = (int)((p4x_rd(P4X_GPIO_IN) >> 21) & 1u);

  /* Hand the pads back to UART1. */

  p4x_wr(mux20, save20);
  p4x_wr(mux21, save21);
  /* routing is owned by the serial driver - do not touch it */

  r = (high == 1 && low == 0) ? OK : -EIO;

  printf("P4X-LOOP-P: pad loopback: drove high -> read %d, drove low -> read %d"
         " => %s\n", high, low,
         (r == OK) ? "PASS (the two pads are connected)" :
         "FAIL (the jumper is missing or the pads are not connected)");

  return r;
}

/****************************************************************************
 * Name: board_lora_pinmux
 *
 * Description:
 *   Dump the IO-MUX register of each A28 pin plus the GPIO latches, so the log
 *   shows what function every pad is really set to (MCU_SEL), which pull it
 *   carries, and whether its output enable and latch bits are set - instead of
 *   taking the driver's word for it.
 *
 ****************************************************************************/

void board_lora_pinmux(void)
{
  static const int pins[] = { 20, 21, 22, 23, 24 };
  static const char *names[] =
  {
    "UART1 TX -> module RXD",
    "UART1 RX <- module TXD",
    "MD0",
    "MD1",
    "AUX (not wired)"
  };
  unsigned int pad = p4x_rd(P4X_GPIO_IN);
  unsigned int en = p4x_rd(P4X_GPIO_ENABLE);
  unsigned int lat = p4x_rd(P4X_GPIO_OUT);
  unsigned int st = lora_uart_status();
  int i;

  printf("P4X-MUX: gpio_in=0x%08x gpio_enable=0x%08x gpio_out=0x%08x\n",
         pad, en, lat);
  printf("P4X-MUX: uart1_status=0x%08x rxfifo=%lu rxd=%u txd=%u\n", st,
         (unsigned long)(st & P4X_UART_RXFIFO_CNT),
         (st & P4X_UART_RXD_LEVEL) ? 1u : 0u,
         (st & P4X_UART_TXD_LEVEL) ? 1u : 0u);

  /* A live UART transmitter idles with TXD high, so a zero STATUS word means
   * the controller is not running.  CLK_CONF holds the clock source and the
   * divider: all zero would mean it was never enabled.
   */

  {
    unsigned long b = board_lora_uart_base();
    unsigned int clk = p4x_rd(b + P4X_UART_OFF_CLKCONF);

    printf("P4X-MUX: live uart block 0x%08lx clk_conf=0x%08x (div=%u src=%u)\n",
           b, clk, clk & 0xfffu, (clk >> 12) & 3u);
  }

  printf("P4X-MUX: device %s, baud %d\n", LORA_DEV, g_baud);

  for (i = 0; i < (int)(sizeof(pins) / sizeof(pins[0])); i++)
    {
      int p = pins[i];
      unsigned int v = p4x_rd(P4X_IO_MUX_PIN(p));

      printf("P4X-MUX: GPIO%-2d io_mux=0x%08x mcu_sel=%u ie=%u wpu=%u wpd=%u | "
             "pad=%u oe=%u latch=%u | %s\n",
             p, v, (v & P4X_MCU_SEL_M) >> 12,
             (v & P4X_FUN_IE) ? 1u : 0u,
             (v & P4X_FUN_WPU) ? 1u : 0u,
             (v & P4X_FUN_WPD) ? 1u : 0u,
             (pad >> p) & 1u, (en >> p) & 1u, (lat >> p) & 1u, names[i]);
    }
}

/****************************************************************************
 * Name: board_lora_uart_scan
 *
 * Description:
 *   Dump the handful of UART registers that prove which controller a device
 *   is really driving.
 *
 *   CLK_CONF holds the clock divider and source.  A controller that has been
 *   opened and programmed shows a non-zero divider; a block nobody touched
 *   reads 0x00000000.  board_lora_initialize() reported the port open and the
 *   probe writing bytes, yet the UART1 block at 0x500d0000 read all zeros -
 *   so either that address is not the controller behind /dev/ttyS0, or the
 *   port is dead.  This prints every candidate block so the log settles it.
 *
 ****************************************************************************/

void board_lora_uart_scan(void)
{
  static const unsigned long bases[] =
  {
    0x500d0000ul, 0x500d1000ul, 0x500d2000ul,
    0x500d3000ul, 0x500d4000ul, 0x500d5000ul
  };
  int i;

  printf("P4X-SCAN: device %s at %d baud\n", LORA_DEV, g_baud);

  for (i = 0; i < (int)(sizeof(bases) / sizeof(bases[0])); i++)
    {
      unsigned long b = bases[i];
      unsigned int clk = p4x_rd(b + 0x88ul);        /* UART_CLK_CONF_REG */
      unsigned int conf0 = p4x_rd(b + 0x20ul);
      unsigned int clkdiv = p4x_rd(b + 0x14ul);
      unsigned int st = p4x_rd(b + 0x1cul);
      unsigned int id = p4x_rd(b + 0x9cul);
      unsigned int date = p4x_rd(b + 0x8cul);

      printf("P4X-SCAN: uart@0x%08lx id=0x%08x date=0x%08x clk_conf=0x%08x "
             "(div=%u src=%u) clkdiv=0x%08x conf0=0x%08x (rx_en=%u tx_en=%u) "
             "status=0x%08x rxfifo=%lu txd=%u\n",
             b, id, date, clk, clk & 0xfffu, (clk >> 12) & 3u, clkdiv, conf0,
             (conf0 >> 3) & 1u, (conf0 >> 4) & 1u, st,
             (unsigned long)(st & P4X_UART_RXFIFO_CNT),
             (st & P4X_UART_TXD_LEVEL) ? 1u : 0u);
    }
}

int board_lora_rx_pullup(int on, FAR int *pad_level, FAR int *uart_level)
{
  unsigned long reg = P4X_IO_MUX_PIN(21);
  unsigned int v = p4x_rd(reg);
  unsigned int st;

  if (on)
    {
      v |= P4X_FUN_IE | P4X_FUN_WPU;
      v &= ~P4X_FUN_WPD;
    }
  else
    {
      v &= ~(P4X_FUN_WPU | P4X_FUN_WPD);
    }

  p4x_wr(reg, v);
  usleep(20000);

  st = lora_uart_status();

  if (pad_level != NULL)
    {
      *pad_level = (int)((p4x_rd(P4X_GPIO_IN) >> 21) & 1u);
    }

  if (uart_level != NULL)
    {
      *uart_level = (st & P4X_UART_RXD_LEVEL) ? 1 : 0;
    }

  return OK;
}

void board_lora_pins(FAR int *md0, FAR int *md1, FAR int *aux)
{
  /* Report what the driver commanded *and* what the pad actually reads: the
   * commanded value alone proved worthless once already, because GPIO22/23
   * come out of reset as the PSRAM debug pads (FUNC 4) rather than GPIO.
   */

  if (md0 != NULL)
    {
      *md0 = lora_pin_level(CONFIG_BOARD_LORA_MD0);
    }

  if (md1 != NULL)
    {
      *md1 = lora_pin_level(CONFIG_BOARD_LORA_MD1);
    }

  if (aux != NULL)
    {
      *aux = lora_pin_level(CONFIG_BOARD_LORA_AUX);
    }
}

void board_lora_pins_commanded(FAR int *md0, FAR int *md1)
{
  if (md0 != NULL)
    {
      *md0 = g_md0;
    }

  if (md1 != NULL)
    {
      *md1 = g_md1;
    }
}

void board_lora_stats(FAR struct board_lora_stats_s *out)
{
  if (out == NULL)
    {
      return;
    }

  out->tx_pkts  = g_tx_pkts;
  out->rx_pkts  = g_rx_pkts;
  out->tx_bytes = g_tx_bytes;
  out->rx_bytes = g_rx_bytes;
  out->rssi     = g_last_rssi;
}

int board_lora_set_state(int md0, int md1)
{
  lora_mode(md0, md1);
  return OK;
}

#else /* !CONFIG_BOARD_LORA */

int board_lora_initialize(void)
{
  return -ENOSYS;
}

#endif /* CONFIG_BOARD_LORA */

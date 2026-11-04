/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_lora.h
 *
 * A28-T2G4A12D1a (SX1281) 2.4 GHz module on UART1.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_LORA_H
#define __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_LORA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct board_lora_stats_s
{
  unsigned long tx_pkts;
  unsigned long rx_pkts;
  unsigned long tx_bytes;
  unsigned long rx_bytes;
  int           rssi;         /* last RSSI read, -127 when unknown */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_lora_initialize
 *
 * Description:
 *   Route MD0/MD1 through the IO-MUX as GPIOs, drive them to the half-duplex
 *   data state (0/0), open UART1 at the factory rate (9600 8N1), then probe
 *   the module with the manual's binary commands (0xC3 / 0xF3 / 0xC1).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lora_initialize(void);

/* P4X-DIAG3: measurement pass. */

int board_lora_diag3(void);

/* P4X-MODPROBE: ask the A28 for its versions and configuration. */

int board_lora_modprobe(void);

/* P4X-MDTEST: MD0/MD1 drive capability and MCU_SEL choice. */

int board_lora_mdtest(void);

/* P4X-FINAL1: pure UART measurement of the 20<->21 jumper. */

int board_lora_final1(void);

/* P4X-BAUDSWEEP2: program UART_CLKDIV directly and listen at each rate. */

int board_lora_baud_sweep2(void);

/* P4X-BEACON: send a visible ASCII beacon. */

int board_lora_beacon_test(void);

/* P4X-BAUDSEARCH: geometric sweep of UART_CLKDIV. */

int board_lora_baud_search(void);

/* P4X-BAUDFIX: program UART_CLKDIV for 9600 baud. */

int board_lora_baud_fix(void);

/* P4X-BAUDCALC: divider computed from HP_SYS_CLKRST. */

int board_lora_baud_calc(void);

/****************************************************************************
 * Name: board_lora_initialize_mode
 *
 * Description:
 *   probe = 1 -> open the port and run the configuration probe (MD0=MD1=1).
 *                While the module is in that state it forwards nothing, and the
 *                probe drains whatever had already arrived, so a receive loop
 *                started afterwards sees an empty line.
 *   probe = 0 -> open the port for data only, leave the module in the
 *                transparent 0/0 state and leave pending bytes in the FIFO.
 *
 *   The ground station uses probe = 0: it only ever wants telemetry.
 *
 ****************************************************************************/

int board_lora_initialize_mode(int probe);

/****************************************************************************
 * Name: board_lora_probe
 *
 * Description:
 *   Enter the 1/1 configuration state, read the hardware version (0xC3), the
 *   software version (0xF3) and the configuration (0xC1), then return to the
 *   half-duplex data state.
 *
 *   An answer only counts when the bytes actually contain "A28", "OK" or
 *   "ERROR" (the strings the manual promises).  A non-empty buffer is not
 *   evidence: this UART has been observed to hand back 25 x 0x00 when there
 *   is nothing on the wire.
 *
 * Returned Value:
 *   Zero (OK) when the module answered; -ENODEV otherwise.
 *
 ****************************************************************************/

int board_lora_probe(void);

/****************************************************************************
 * Name: board_lora_probe_baud
 *
 * Description:
 *   Switch the port to `baud` and re-run the full probe.
 *
 * Returned Value:
 *   Zero (OK) when the module answered at that rate; -ENODEV if it did not.
 *
 ****************************************************************************/

int board_lora_probe_baud(int baud);

/****************************************************************************
 * Name: board_lora_sweep
 *
 * Description:
 *   Try every baud rate the SPEED register supports (manual 6.2), factory
 *   9600 first.  On success the port is left at the rate that answered.
 *
 * Input Parameters:
 *   report - optional array receiving the rate(s) that produced an answer
 *   max    - capacity of `report`
 *
 * Returned Value:
 *   The number of rates that answered (0 = the module never answered).
 *
 ****************************************************************************/

int board_lora_sweep(FAR int *report, int max);

/****************************************************************************
 * Name: board_lora_send / board_lora_recv
 *
 * Description:
 *   Transparent-mode data path: what is written is what goes on air, and what
 *   comes off air is what is read.  Only meaningful in the 0/0 state.
 *
 ****************************************************************************/

int board_lora_send(FAR const void *data, size_t len);
int board_lora_recv(FAR void *buf, size_t len);

/****************************************************************************
 * Name: board_lora_ready / board_lora_detected
 *
 * Description:
 *   ready     - the port is open
 *   detected  - the module answered a version request with a valid string
 *
 ****************************************************************************/

int board_lora_ready(void);
int board_lora_detected(void);

/****************************************************************************
 * Name: board_lora_baud
 *
 * Description:
 *   The rate the port is currently programmed to.
 *
 ****************************************************************************/

int board_lora_baud(void);

/****************************************************************************
 * Name: board_lora_hwversion / board_lora_version
 *
 * Description:
 *   Hardware ("A28-T2G4A12D1a-V9.0") and software ("A28-T2G4A12D1a-V9.00")
 *   version strings as reported by the module, or "(no answer)".
 *
 ****************************************************************************/

FAR const char *board_lora_hwversion(void);
FAR const char *board_lora_version(void);

/****************************************************************************
 * Name: board_lora_config
 *
 * Description
 *   The five configuration bytes ADDH ADDL SPEED CHAN OPTION (the 0xC0 echo
 *   byte of the raw answer is stripped), or NULL when the read failed.
 *
 ****************************************************************************/

const uint8_t *board_lora_config(void);

/****************************************************************************
 * Name: board_lora_line
 *
 * Description:
 *   What the UART hardware itself sees on the RX line:
 *     rxd_level - UART_STATUS[15], the level of the RXD signal (1 = idle high)
 *     rx_fifo   - UART_STATUS[7:0], bytes waiting in the RX FIFO
 *
 *   rxd_level = 0 with rx_fifo = 0 means the line is being held low, which is
 *   a wiring or module-power fault, not a silent module.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV when the port is not open.
 *
 ****************************************************************************/

int board_lora_line(FAR int *rxd_level, FAR int *rx_fifo);

/****************************************************************************
 * Name: board_lora_rx_pullup
 *
 * Description:
 *   Switch the internal pull-up on the RX pad (GPIO21) on or off and report
 *   the pad level and the level the UART sees.
 *
 *   A floating pad reads low, so "rxd_level = 0" alone cannot distinguish a
 *   floating wire from a shorted one.  With the internal pull-up engaged a
 *   floating line reads 1, while a line held low by something outside the chip
 *   stays 0.
 *
 ****************************************************************************/

int board_lora_rx_pullup(int on, FAR int *pad_level, FAR int *uart_level);

/****************************************************************************
 * Name: board_lora_route
 *
 * Description:
 *   Program the ESP32-P4 GPIO matrix so that UART1 TX drives `tx_pin` (GPIO20)
 *   and UART1 RX is fed from `rx_pin` (GPIO21), then read both registers back
 *   and verify them.
 *
 *   Needed because the image this driver runs in never programmed the matrix:
 *   func_out_sel_cfg[GPIO20] still held its reset value 0x100 (= 256, no
 *   signal attached), so bytes written to /dev/ttyS0 never reached the pad and
 *   the module was unreachable no matter how it was wired.  board_lora_
 *   initialize() calls this; call it again after any code that reprograms the
 *   matrix (for example a UART loopback test).
 *
 * Returned Value:
 *   Zero (OK) when both registers read back as programmed; -EIO otherwise.
 *
 ****************************************************************************/

int board_lora_route(int rx_pin, int tx_pin);

/****************************************************************************
 * Name: board_lora_tx_probe
 *
 * Description:
 *   Write 16 bytes and sample, while they go out, both the level the UART
 *   believes it transmits (UART_STATUS[31], UART_TXD) and the level the TX pad
 *   (GPIO20) really has.
 *
 * Output Parameters:
 *   txd_bit   - the UART's own TXD bit at the end of the burst
 *   pad_level - GPIO20's pad level at the end of the burst
 *   moved     - 1 when both a low and a high were observed on either side
 *   bytes     - bytes accepted by write()
 *
 ****************************************************************************/

int board_lora_tx_probe(FAR int *txd_bit, FAR int *pad_level,
                        FAR int *moved, FAR int *bytes);

/****************************************************************************
 * Name: board_lora_matrix_loopback
 *
 * Description:
 *   Route UART1 TX back into UART1 RX inside the GPIO matrix (no pad involved)
 *   and send `len` bytes from `pat`.
 *
 * Output Parameters:
 *   out    - buffer for whatever came back
 *   got    - number of bytes received
 *
 * Returned Value:
 *   Zero (OK) when the transfer ran; -ENODEV when the port is closed.
 *
 ****************************************************************************/

int board_lora_matrix_loopback(FAR const uint8_t *pat, int len,
                               FAR uint8_t *out, int outmax, int *got);

/****************************************************************************
 * Name: board_lora_pad_loopback
 *
 * Description:
 *   Drive GPIO20 as a plain GPIO and read GPIO21 back with a jumper between
 *   them; checks the pads and header pins without the UART.
 *
 * Returned Value:
 *   Zero (OK) when the driven level appeared on GPIO21; -EIO otherwise.
 *
 ****************************************************************************/

int board_lora_pad_loopback(void);

/****************************************************************************
 * Name: board_lora_pins
 *
 * Description:
 *   The MD0/MD1 levels the driver last drove and the AUX level read back.
 *
 ****************************************************************************/

void board_lora_pins(FAR int *md0, FAR int *md1, FAR int *aux);

/****************************************************************************
 * Name: board_lora_pins_commanded
 *
 * Description:
 *   The MD0/MD1 levels the driver last asked for (as opposed to the levels the
 *   pads actually read, which board_lora_pins() reports).
 *
 ****************************************************************************/

void board_lora_pins_commanded(FAR int *md0, FAR int *md1);

/****************************************************************************
 * Name: board_lora_pinmux
 *
 * Description:
 *   Print the IO-MUX, output-enable and latch state of the five A28 pins plus
 *   the UART1 status register.
 *
 ****************************************************************************/

void board_lora_pinmux(void);

/****************************************************************************
 * Name: board_lora_uart_scan
 *
 * Description:
 *   Print the clock/conf/status registers of the first six UART register
 *   blocks, to show which controller the open device is really driving.
 *
 ****************************************************************************/

void board_lora_uart_scan(void);

/****************************************************************************
 * Name: board_lora_stats
 *
 * Description:
 *   Packet/byte counters for the UI.
 *
 ****************************************************************************/

void board_lora_stats(FAR struct board_lora_stats_s *out);

/****************************************************************************
 * Name: board_lora_set_state
 *
 * Description:
 *   Drive MD0/MD1: 0/0 half duplex, 0/1 or 1/0 full duplex, 1/1 configuration.
 *
 ****************************************************************************/

int board_lora_set_state(int md0, int md1);

/* P4X-SHORTTEST: hardware check of the 37 <-> 21 shorting wire. */

int board_lora_short_probe(void);

/* P4X-RXINDEX: which GPIO-matrix input slot feeds UART0's receiver? */

int board_lora_rx_index_probe(void);

/* P4X-RXDIAG: TX pad drive + all-256-slot receiver sweep. */

int board_lora_rx_diag(void);

/* P4X-UARTLB: receiver switch hunt through every P4-specific enable bit. */

int board_lora_uart_lbtest(void);

/* P4X-RXPOP: does reading the data port pop the RX SRAM ring? */

int board_lora_rx_pop_probe(void);

/* P4X-EXTPATH: TX pad / RX pad / receiver input sampled in one pass. */

int board_lora_extpath_probe(void);

/* P4X-ERRMASK: clear err_wr_mask and look at the frame-error interrupts. */

int board_lora_errwrmask_probe(void);

/* P4X-BITBANG: hand-clock a frame onto a pad and see who receives it. */

int board_lora_bitbang_probe(void);

/* P4X-RXSTEPS: send three bytes, read each one back immediately. */

int board_lora_rx_steps_probe(void);

#endif /* __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_LORA_H */

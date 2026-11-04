/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_pins.h
 *
 * Pin facts for ESP32-P4X-Function-EV-Board.
 * Sources: Espressif user guide (J1 header table) and the community
 * schematic audit of the V1.8 board.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ESP32P4X_BOARD_PINS_H
#define __VENDOR_ESP32P4X_BOARD_PINS_H

/* J1 expansion header (verified against the Espressif user guide) */

#define BOARD_J1_U0TXD_GPIO    37   /* J1 pin 8  - UART0 TX   */
#define BOARD_J1_U0RXD_GPIO    38   /* J1 pin 10 - UART0 RX   */
#define BOARD_J1_LCD_PWM_GPIO  26   /* J1 pin 31 - LCD backlight PWM */
#define BOARD_J1_LCD_RST_GPIO  27   /* J1 pin 38 - LCD reset (RST_LCD) */
#define BOARD_J1_USER_OUT_GPIO 48   /* J1 pin 33 - no alternate function */

/* On-board BOOT button */
#define BOARD_BUTTON_BOOT_GPIO 35

/* Pins that must NOT be used as free GPIO (community schematic audit):
 *
 *   GPIO0/GPIO1   : 32.768 kHz crystal (R61/R59)
 *   GPIO7/GPIO8   : shared on-board I2C (SCL/SDA), ES8311 + camera + touch
 *   GPIO14..19    : ESP32-C6 SDIO bus
 *   GPIO6         : C6_WAKEUP
 *   GPIO54        : C6_EN
 *   GPIO35        : also drives RMII_TXD1 when Ethernet is enabled
 */

#endif /* __VENDOR_ESP32P4X_BOARD_PINS_H */

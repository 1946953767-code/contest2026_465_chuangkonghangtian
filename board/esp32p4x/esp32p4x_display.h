/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4x-function-ev-board/src/esp32p4x_display.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4X_FUNCTION_EV_BOARD_SRC_ESP32P4X_DISPLAY_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4X_FUNCTION_EV_BOARD_SRC_ESP32P4X_DISPLAY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* EK79007 7" MIPI-DSI panel fitted to the ESP32-P4X / ESP32-P4 Function EV
 * board carrier.  These values come from the locally verified bring-up of
 * this exact panel/board combination, not from a datasheet guess.
 */

#define ESP32P4X_LCD_WIDTH              1024
#define ESP32P4X_LCD_HEIGHT             600
#define ESP32P4X_LCD_BPP                16   /* RGB565 */
#define ESP32P4X_LCD_STRIDE \
  (ESP32P4X_LCD_WIDTH * (ESP32P4X_LCD_BPP / 8))

#define ESP32P4X_LCD_RESET_GPIO         27   /* J1 pin 38 */
#define ESP32P4X_LCD_BACKLIGHT_GPIO     26   /* J1 pin 31 */

#define ESP32P4X_LCD_DSI_LANES          2
#define ESP32P4X_LCD_DSI_BITRATE_MBPS   1000

/* Panel DPI timing (EK79007, 1024x600):
 *
 *   htotal = 1024 + 10 + 120 + 120 = 1274
 *   vtotal =  600 +  1 +  20 +  10 =  631
 *   1274 * 631 * 60 Hz = 48.2 MHz  ->  48 MHz DPI clock ~= 60 Hz refresh
 */

#define ESP32P4X_LCD_HSW                10
#define ESP32P4X_LCD_HBP                120
#define ESP32P4X_LCD_HFP                120
#define ESP32P4X_LCD_VSW                1
#define ESP32P4X_LCD_VBP                20
#define ESP32P4X_LCD_VFP                10
#define ESP32P4X_LCD_DPI_CLK_MHZ        48

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4x_display_initialize
 *
 * Description:
 *   Initialize the MIPI-DSI host for the EK79007 panel.  Called from
 *   esp_bringup() before the framebuffer is created.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int esp32p4x_display_initialize(void);

int esp32p4x_panel_reinit(void);

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4X_FUNCTION_EV_BOARD_SRC_ESP32P4X_DISPLAY_H */

/****************************************************************************
 * Native ESP32-P4 DSI driver API (arch/risc-v/src/esp32p4/esp32p4_mipi_dsi.c)
 *
 * Declared here rather than by including <esp32p4/esp32p4_mipi_dsi.h>: that
 * header lives in arch/risc-v/include, which cannot be added to the board's
 * include path without breaking the board build (it drags in NuttX kernel
 * headers that clash with the openvela ones).
 *
 * Layouts are copied verbatim from that header so the ABI matches.
 ****************************************************************************/

enum esp32p4_dsi_color_e
{
  ESP32P4_DSI_COLOR_RGB565 = 0,
  ESP32P4_DSI_COLOR_RGB888 = 1,
};

struct esp32p4_dsi_timing_s
{
  uint32_t hsw;
  uint32_t hbp;
  uint32_t width;
  uint32_t hfp;
  uint32_t vsw;
  uint32_t vbp;
  uint32_t height;
  uint32_t vfp;
  float    dpi_clk_mhz;
};

struct esp32p4_dsi_config_s
{
  uint8_t                  num_lanes;
  float                    lane_bit_rate_mbps;
  enum esp32p4_dsi_color_e color_format;
  struct esp32p4_dsi_timing_s timing;
};

int esp32p4_mipi_dsi_initialize(FAR const struct esp32p4_dsi_config_s *config);
int esp32p4_mipi_dsi_set_framebuffer(uintptr_t fb_addr, size_t fb_size);
int esp32p4_mipi_dsi_start_video(void);
int esp32p4_mipi_dsi_stop_video(void);

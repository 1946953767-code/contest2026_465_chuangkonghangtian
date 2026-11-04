# ##############################################################################
# vendor/espressif/boards/esp32p4x-function-ev-board/src/CMakeLists.txt
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ##############################################################################

set(SRCS board_boot.c board_bringup.c)

if(CONFIG_DEV_GPIO)
  list(APPEND SRCS board_gpio.c)
endif()

if(CONFIG_BOARDCTL_RESET)
  list(APPEND SRCS board_reset.c)
endif()

if(CONFIG_ESP32P4X_MIPI_DSI)
  # Official NuttX implementation for this exact board, ported verbatim:
  # it uses the upstream esp_ldo + esp_mipi_dsi API chain (LDO channel 3 for
  # VDD_MIPI_DPHY, then the DSI host, then the EK79007 panel + /dev/fb0).
  list(APPEND SRCS esp32p4x_display.c)
endif()

if(CONFIG_INPUT_GT9XX)
  list(APPEND SRCS board_touch.c)
endif()

if(CONFIG_BOARD_LORA)
  list(APPEND SRCS board_lora.c)
endif()

if(CONFIG_BOARD_AUDIO)
  list(APPEND SRCS board_audio.c)
endif()

if(CONFIG_ESP32P4X_MIPI_DSI)
  list(APPEND SRCS board_panel.c)
endif()

target_sources(board PRIVATE ${SRCS})

target_include_directories(board PRIVATE ${NUTTX_BOARD_ABS_DIR}/include
                                        ${CMAKE_CURRENT_SOURCE_DIR})

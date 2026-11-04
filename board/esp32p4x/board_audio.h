/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_audio.h
 *
 * ES8311 speaker + microphone on I2S0.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_AUDIO_H
#define __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: board_audio_initialize
 *
 * Description:
 *   Bring up I2S0 and the ES8311 codec on I2C0 and register the audio device
 *   as /dev/audio/pcm0p (playback) and /dev/audio/pcm0c (capture).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_audio_initialize(void);

/* Non-zero once the codec is registered. */

int board_audio_ready(void);

#endif /* __BOARDS_ESP32P4X_FUNCTION_EV_BOARD_SRC_BOARD_AUDIO_H */

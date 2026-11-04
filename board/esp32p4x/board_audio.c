/****************************************************************************
 * vendor/espressif/boards/esp32p4x-function-ev-board/src/board_audio.c
 *
 * ES8311 audio codec bring-up for ESP32-P4X-Function-EV-Board.
 *
 * Hardware (official ESP-BSP for this board):
 *   ES8311 on the on-board I2C0 (SDA = GPIO7, SCL = GPIO8), address 0x18
 *   I2S0 pins: MCLK = GPIO13, BCLK = GPIO12, WS = GPIO10, DOUT = GPIO9 (to the
 *   codec), DIN = GPIO11 (from the codec's ADC = the on-board microphone)
 *
 * The driver follows the upstream NuttX pattern for this codec: get an I2S
 * instance, get the I2C bus, hand both to es8311_initialize(), wrap the result
 * in a PCM decoder and register it, which yields
 *
 *   /dev/audio/pcm0p   playback  (speaker)
 *   /dev/audio/pcm0c   capture   (microphone)
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/i2c/i2c_master.h>

#include "espressif/esp_i2c.h"
#include "espressif/esp_gpio.h"
#include "esp32p4_i2s.h"

#include "board_audio.h"

#ifdef CONFIG_BOARD_AUDIO

#ifndef CONFIG_BOARD_AUDIO_I2C_PORT
#  define CONFIG_BOARD_AUDIO_I2C_PORT 0
#endif

#ifndef CONFIG_BOARD_AUDIO_I2C_ADDR
#  define CONFIG_BOARD_AUDIO_I2C_ADDR 0x18
#endif

#ifndef CONFIG_BOARD_AUDIO_I2C_FREQ
#  define CONFIG_BOARD_AUDIO_I2C_FREQ 100000
#endif

#ifndef CONFIG_BOARD_AUDIO_I2S_PORT
#  define CONFIG_BOARD_AUDIO_I2S_PORT 0
#endif

/* P4X-SPK: the on-board amplifier has to be enabled explicitly - ES8311 can
 * initialise perfectly with the PA still off, which is exactly the silent
 * "ready" state the board was in.
 */

#ifndef CONFIG_BOARD_AUDIO_PA_PIN
#  define CONFIG_BOARD_AUDIO_PA_PIN 53
#endif

static bool g_audio_inited;

/* --------------------------------------------------------------- helpers -- */

/* es8311_initialize() needs only the address and bus speed; the reset and PA
 * pins of the reference boards do not exist here (the codec is reset by the
 * board power-on sequence).
 */

static void board_audio_lower(FAR struct es8311_lower_s *lower)
{
  memset(lower, 0, sizeof(*lower));
  lower->address   = CONFIG_BOARD_AUDIO_I2C_ADDR;
  lower->frequency = CONFIG_BOARD_AUDIO_I2C_FREQ;
}

/* ------------------------------------------------------------ public API -- */

int board_audio_initialize(void)
{
  static struct es8311_lower_s lower;
  FAR struct audio_lowerhalf_s *es8311;
  FAR struct audio_lowerhalf_s *pcm;
  FAR struct i2s_dev_s *i2s;
  FAR struct i2c_master_s *i2c;
  int ret;

  if (g_audio_inited)
    {
      return OK;
    }

  board_audio_lower(&lower);

  /* P4X-SPK: enable the speaker amplifier before touching the codec. */

  (void)esp_configgpio(CONFIG_BOARD_AUDIO_PA_PIN, OUTPUT);
  (void)esp_gpiowrite(CONFIG_BOARD_AUDIO_PA_PIN, 1);
  printf("P4X-AUDIO: PA enable on GPIO%d\n", CONFIG_BOARD_AUDIO_PA_PIN);

  i2s = esp32p4_i2sbus_initialize(CONFIG_BOARD_AUDIO_I2S_PORT);
  if (i2s == NULL)
    {
      printf("P4X-AUDIO: I2S%d unavailable\n", CONFIG_BOARD_AUDIO_I2S_PORT);
      return -ENODEV;
    }

  i2c = esp_i2cbus_initialize(CONFIG_BOARD_AUDIO_I2C_PORT);
  if (i2c == NULL)
    {
      printf("P4X-AUDIO: I2C%d unavailable\n", CONFIG_BOARD_AUDIO_I2C_PORT);
      return -ENODEV;
    }

  es8311 = es8311_initialize(i2c, i2s, &lower);
  if (es8311 == NULL)
    {
      printf("P4X-AUDIO: ES8311 init failed (addr 0x%02x)\n",
             CONFIG_BOARD_AUDIO_I2C_ADDR);
      return -ENODEV;
    }

  pcm = pcm_decode_initialize(es8311);
  if (pcm == NULL)
    {
      printf("P4X-AUDIO: PCM decoder init failed\n");
      return -ENODEV;
    }

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      printf("P4X-AUDIO: audio_register failed: %d\n", ret);
      return ret;
    }

  g_audio_inited = true;
  printf("P4X-AUDIO: ES8311 ready -> /dev/audio/pcm0p (speaker) "
         "/dev/audio/pcm0c (mic), I2S%d pins MCLK=13 BCLK=12 WS=10 DOUT=9 "
         "DIN=11\n", CONFIG_BOARD_AUDIO_I2S_PORT);
  return OK;
}

int board_audio_ready(void)
{
  return g_audio_inited ? 1 : 0;
}

#else /* !CONFIG_BOARD_AUDIO */

int board_audio_initialize(void)
{
  return -ENOSYS;
}

int board_audio_ready(void)
{
  return 0;
}

#endif /* CONFIG_BOARD_AUDIO */

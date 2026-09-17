/****************************************************************************
 * SF32LB52 软件 I2C (GPIO bit-bang) 触摸总线驱动
 * 触摸 FT6146: SCL=PA30, SDA=PA33
 * 说明: OD 模式下 HAL_GPIO_ReadPin 读的是驱动寄存器而非引脚电平,
 *       因此用 "拉低=输出0 / 释放=输入(高阻,靠上拉)" 动态切方向模拟开漏.
 ****************************************************************************/
#include <nuttx/config.h>
#include <errno.h>
#include <nuttx/i2c/i2c_bitbang.h>
#include <nuttx/i2c/i2c_master.h>

#include "bsp_board.h"
#include <sifli_gpio.h>

#define BB_SCL_PIN   (30)   /* PA30 */
#define BB_SDA_PIN   (33)   /* PA33 */

struct sifli_bb_lower_s
{
  struct i2c_bitbang_lower_dev_s lower;
};

static void bb_initialize(FAR struct i2c_bitbang_lower_dev_s *lower)
{
  /* 引脚切回 GPIO 并上拉 */
  HAL_PIN_Set(PAD_PA30, GPIO_A30, PIN_PULLUP, 1);
  HAL_PIN_Set(PAD_PA33, GPIO_A33, PIN_PULLUP, 1);

  /* 默认输入(高阻), 总线空闲为高 */
  sifli_gpio_config(BB_SCL_PIN, GPIO_INPUT);
  sifli_gpio_config(BB_SDA_PIN, GPIO_INPUT);
}

static void bb_set_scl(FAR struct i2c_bitbang_lower_dev_s *lower, bool high)
{
  if (high)
    {
      /* 释放: 输入(高阻), 由上拉拉高 */
      sifli_gpio_config(BB_SCL_PIN, GPIO_INPUT);
    }
  else
    {
      sifli_gpio_config(BB_SCL_PIN, GPIO_OUTPUT);
      sifli_gpio_write(BB_SCL_PIN, false);
    }
}

static void bb_set_sda(FAR struct i2c_bitbang_lower_dev_s *lower, bool high)
{
  if (high)
    {
      sifli_gpio_config(BB_SDA_PIN, GPIO_INPUT);
    }
  else
    {
      sifli_gpio_config(BB_SDA_PIN, GPIO_OUTPUT);
      sifli_gpio_write(BB_SDA_PIN, false);
    }
}

static bool bb_get_scl(FAR struct i2c_bitbang_lower_dev_s *lower)
{
  return sifli_gpio_read(BB_SCL_PIN);
}

static bool bb_get_sda(FAR struct i2c_bitbang_lower_dev_s *lower)
{
  return sifli_gpio_read(BB_SDA_PIN);
}

static const struct i2c_bitbang_lower_ops_s g_bb_ops =
{
  bb_initialize,
  bb_set_scl,
  bb_set_sda,
  bb_get_scl,
  bb_get_sda
};

static struct sifli_bb_lower_s g_sifli_bb_touch =
{
  { &g_bb_ops, NULL }
};

static FAR struct i2c_master_s *g_bb_master = NULL;

FAR struct i2c_master_s *sifli_bitbang_touch_master(void)
{
  if (g_bb_master == NULL)
    {
      g_bb_master = i2c_bitbang_initialize(&g_sifli_bb_touch.lower);
    }

  return g_bb_master;
}

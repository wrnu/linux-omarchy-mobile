// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIOs of the Samsung S2MPG11 PMIC
 *
 * The S2MPG11 sub-PMIC paired with Google's gs101 SoC has six GPIOs. Unlike
 * the S2MPG13's, they have no register block of their own: seven registers
 * at the end of the PMIC block hold one bit per pin each (input level,
 * output level, output enable, pull-down, pull-up, drive strength and
 * remote control), reached through the ACPM firmware like the rest of the
 * PMIC. On the Pixel 6 boards they switch the camera PMIC and the
 * telephoto camera's supply.
 *
 * The register layout is from Google's GPL s2mpg11 register header and its
 * s2mpg1x GPIO driver. Only the output level and output enable bits are
 * ever changed; the pulls, drive strength and remote control stay as the
 * bootloader left them.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/mfd/samsung/s2mpg11.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define S2MPG11_GPIO_NUM	6

#define S2MPG11_GPIO_IN		S2MPG11_PMIC_GPIO_CTRL1
#define S2MPG11_GPIO_OUT	S2MPG11_PMIC_GPIO_CTRL2
#define S2MPG11_GPIO_OUT_EN	S2MPG11_PMIC_GPIO_CTRL3
#define S2MPG11_GPIO_REMOTE	S2MPG11_PMIC_GPIO_CTRL7

struct s2mpg11_gpio {
	struct gpio_chip gc;
	struct regmap *regmap;
};

static int s2mpg11_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg11_gpio *chip = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, S2MPG11_GPIO_OUT_EN, &val);
	if (ret)
		return ret;

	return (val & BIT(offset)) ? GPIO_LINE_DIRECTION_OUT :
				     GPIO_LINE_DIRECTION_IN;
}

static int s2mpg11_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg11_gpio *chip = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = s2mpg11_gpio_get_direction(gc, offset);
	if (ret < 0)
		return ret;

	/* An output reads back the level it drives. */
	ret = regmap_read(chip->regmap, ret == GPIO_LINE_DIRECTION_OUT ?
			  S2MPG11_GPIO_OUT : S2MPG11_GPIO_IN, &val);
	if (ret)
		return ret;

	return !!(val & BIT(offset));
}

static int s2mpg11_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct s2mpg11_gpio *chip = gpiochip_get_data(gc);

	return regmap_assign_bits(chip->regmap, S2MPG11_GPIO_OUT, BIT(offset),
				  value);
}

static int s2mpg11_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct s2mpg11_gpio *chip = gpiochip_get_data(gc);

	return regmap_clear_bits(chip->regmap, S2MPG11_GPIO_OUT_EN, BIT(offset));
}

static int s2mpg11_gpio_direction_output(struct gpio_chip *gc,
					 unsigned int offset, int value)
{
	struct s2mpg11_gpio *chip = gpiochip_get_data(gc);
	int ret;

	/* The level first, so the pin never drives the old one. */
	ret = regmap_assign_bits(chip->regmap, S2MPG11_GPIO_OUT, BIT(offset),
				 value);
	if (ret)
		return ret;

	return regmap_set_bits(chip->regmap, S2MPG11_GPIO_OUT_EN, BIT(offset));
}

/* Log the pins as the bootloader left them; reads only. */
static void s2mpg11_gpio_log_state(struct device *dev, struct regmap *regmap)
{
	unsigned int in, out, out_en, remote;

	if (regmap_read(regmap, S2MPG11_GPIO_IN, &in) ||
	    regmap_read(regmap, S2MPG11_GPIO_OUT, &out) ||
	    regmap_read(regmap, S2MPG11_GPIO_OUT_EN, &out_en) ||
	    regmap_read(regmap, S2MPG11_GPIO_REMOTE, &remote)) {
		dev_warn(dev, "can't read the GPIO state\n");
		return;
	}

	dev_info(dev, "in %#04x out %#04x out-en %#04x remote %#04x\n",
		 in, out, out_en, remote);
}

static int s2mpg11_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2mpg11_gpio *chip;

	if (!dev_fwnode(dev))
		return dev_err_probe(dev, -ENODEV, "no device tree node\n");

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->regmap = dev_get_regmap(dev->parent, "pmic");
	if (!chip->regmap)
		return dev_err_probe(dev, -ENODEV, "no PMIC regmap\n");

	s2mpg11_gpio_log_state(dev, chip->regmap);

	chip->gc.label = dev_name(dev);
	chip->gc.parent = dev;
	chip->gc.owner = THIS_MODULE;
	chip->gc.base = -1;
	chip->gc.ngpio = S2MPG11_GPIO_NUM;
	chip->gc.can_sleep = true;
	chip->gc.get_direction = s2mpg11_gpio_get_direction;
	chip->gc.direction_input = s2mpg11_gpio_direction_input;
	chip->gc.direction_output = s2mpg11_gpio_direction_output;
	chip->gc.get = s2mpg11_gpio_get;
	chip->gc.set = s2mpg11_gpio_set;

	return devm_gpiochip_add_data(dev, &chip->gc, chip);
}

static const struct of_device_id s2mpg11_gpio_of_match[] = {
	{ .compatible = "samsung,s2mpg11-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpg11_gpio_of_match);

static const struct platform_device_id s2mpg11_gpio_id[] = {
	{ "s2mpg11-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(platform, s2mpg11_gpio_id);

static struct platform_driver s2mpg11_gpio_driver = {
	.driver = {
		.name = "s2mpg11-gpio",
		.of_match_table = s2mpg11_gpio_of_match,
	},
	.probe = s2mpg11_gpio_probe,
	.id_table = s2mpg11_gpio_id,
};
module_platform_driver(s2mpg11_gpio_driver);

MODULE_DESCRIPTION("Samsung S2MPG11 PMIC GPIO driver");
MODULE_LICENSE("GPL");

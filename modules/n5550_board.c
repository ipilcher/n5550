/*
 * Copyright 2013, 2021 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/leds.h>
#include <linux/leds-pca9532.h>
#include <linux/i2c.h>
#include <linux/pci.h>
#include <linux/gpio.h>

#define N5550_PCA9532_1_GPIO_BASE	16
#define N5550_BOARD_ID			2

static int ich_gpio_base = -1;
module_param(ich_gpio_base, uint, 0444);
MODULE_PARM_DESC(ich_gpio_base,
		 "GPIO base of ICH10R - default is -1 (auto-detect)");
/*
 * Disk activity LEDs are controlled by GPIO pins on the ICH10R chipset
 */

static struct gpio_led n5550_ich_gpio_leds[5] = {
	{
		.name			= "n5550:green:disk-act-0",
		.default_trigger	= "n5550-ahci-0",
		.active_low		= 1,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
	},
	{
		.name			= "n5550:green:disk-act-1",
		.default_trigger	= "n5550-ahci-1",
		.active_low		= 1,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
	},
	{
		.name			= "n5550:green:disk-act-2",
		.default_trigger	= "n5550-ahci-2",
		.active_low		= 1,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
	},
	{
		.name			= "n5550:green:disk-act-3",
		.default_trigger	= "n5550-ahci-3",
		.active_low		= 1,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
	},
	{
		.name			= "n5550:green:disk-act-4",
		.default_trigger	= "n5550-ahci-4",
		.active_low		= 1,
		.default_state		= LEDS_GPIO_DEFSTATE_OFF,
	},
};

static struct gpio_led_platform_data n5550_ich_gpio_led_data = {
	.num_leds			= ARRAY_SIZE(n5550_ich_gpio_leds),
	.leds				= n5550_ich_gpio_leds,
};

static struct platform_device n5550_ich_gpio_led_pdev = {
	.name				= "leds-gpio",
	.id				= -1,
	.dev.platform_data		= &n5550_ich_gpio_led_data,
};

/* GPIO numbers of LEDS are not contiguous */
static const unsigned __initdata n5550_ich_gpio_led_offsets[5] = {
	0, 2, 3, 4, 5,
};

static int __init n5550_match_ich_gpiochip(struct gpio_chip *const gc,
				    __attribute__((unused)) void *data)
{
	/* Can gc->label be NULL? */
	return (gc->label != NULL) && !strcmp(gc->label, "gpio_ich");
}

static int __init n5550_get_ich_gpiobase(void)
{
	const struct gpio_chip *gc;

	if ((gc = gpiochip_find(NULL, n5550_match_ich_gpiochip)) == NULL) {
		pr_warn("Couldn't find ICH GPIO chip\n");
		return -ENODEV;
	}

	if (gc->base < 0) {
		pr_warn("ICH GPIO chip has invalid base (%d)\n", gc->base);
		return -EINVAL;
	}

	pr_debug("n5550_board: ICH GPIO base: %d\n", gc->base);

	return gc->base;
}

static int __init n5550_ich_gpio_led_setup(void)
{
	unsigned i;
	int base;

	if (ich_gpio_base < 0) {
		if ((base = n5550_get_ich_gpiobase()) < 0)
			return base;
	}
	else {
		base = ich_gpio_base;
	}

	for (i = 0; i < ARRAY_SIZE(n5550_ich_gpio_leds); ++i) {
		n5550_ich_gpio_leds[i].gpio = base +
					      n5550_ich_gpio_led_offsets[i];
	}

	return platform_device_register(&n5550_ich_gpio_led_pdev);
}

static void n5550_ich_gpio_led_cleanup(void)
{
	platform_device_unregister(&n5550_ich_gpio_led_pdev);
}

/*
 * The N5550 BIOS does not correctly mark the GPIO pins as usable
 */

/* PCI ID of the ICH10R LPC controller */
#define N5550_ICH_PCI_VENDOR		PCI_VENDOR_ID_INTEL
#define N5550_ICH_LPC_PCI_DEV		0x3a16
#define N5550_ICH_I2C_PCI_DEV		0x3a30

/* PCI configuration registers - from drivers/mfd/lpc_ich.c */
#define N5550_ICH_PCI_GPIO_BASE        	0x48
#define N5550_ICH_PCI_GPIO_CTRL        	0x4c

/* I/O port offsets - from drivers/gpio/gpio-ich.c */
#define N5550_ICH_GPIO_USE_SEL_0     	0x00
#define N5550_ICH_GPIO_USE_SEL_1     	0x30
#define N5550_ICH_GPIO_USE_SEL_2	0x40

/* Enable GPIO pins 0, 2, 3, 4, 5, 9, 28, and 34 */
#define N5550_ICH_GPIO_PINS_0	(			\
					(1 << 0) |	\
					(1 << 2) |	\
					(1 << 3) |	\
					(1 << 4) |	\
					(1 << 5) |	\
					(1 << 9) |	\
					(1<< 28)	\
				)
#define N5550_ICH_GPIO_PINS_1		(1 << (34 - 32))

static int __init n5550_ich_gpio_setup(void)
{
	struct pci_dev *dev;
	u32 gpio_io_base, gpio_pins;

	dev = pci_get_device(N5550_ICH_PCI_VENDOR, N5550_ICH_LPC_PCI_DEV, NULL);
	if (dev == NULL)
		return -ENODEV;

	pci_read_config_dword(dev, N5550_ICH_PCI_GPIO_BASE, &gpio_io_base);
	gpio_io_base &= 0x0000ff80;

	/* Ensure ICH GPIO function is on */
	pci_write_config_byte(dev, N5550_ICH_PCI_GPIO_CTRL, 0x10);

	gpio_pins = inl(gpio_io_base + N5550_ICH_GPIO_USE_SEL_0);
	gpio_pins |= N5550_ICH_GPIO_PINS_0;
	outl(gpio_pins, gpio_io_base + N5550_ICH_GPIO_USE_SEL_0);

	gpio_pins = inl(gpio_io_base + N5550_ICH_GPIO_USE_SEL_1);
	gpio_pins |= N5550_ICH_GPIO_PINS_1;
	outl(gpio_pins, gpio_io_base + N5550_ICH_GPIO_USE_SEL_1);

	pci_dev_put(dev);
	return 0;
}

/*
 * Other LEDs are controlled by 2 NXP PCA9532 dimmers
 */

#define N5550_BOARD_ID_GPIO_0		N5550_PCA9532_1_GPIO_BASE
#define N5550_BOARD_ID_GPIO_1		(N5550_PCA9532_1_GPIO_BASE + 1)
#define N5550_BOARD_ID_GPIO_2		(N5550_PCA9532_1_GPIO_BASE + 2)
#define N5550_BOARD_ID_GPIO_3		(N5550_PCA9532_1_GPIO_BASE + 3)
#define N5550_BOARD_PIC_GPIO		(N5550_PCA9532_1_GPIO_BASE + 15)

static struct pca9532_platform_data n5550_pca9532_0_pdata = {
	.leds 	= {
			{
				.name 	= "n5550:red:disk-stat-0",
				.type 	= PCA9532_TYPE_LED,
				.state 	= PCA9532_OFF,
			},
			{
				.name	= "n5550:red:disk-stat-1",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
			{
				.name	= "n5550:red:disk-stat-2",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
			{
				.name	= "n5550:red:disk-stat-3",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
			{
				.name	= "n5550:red:disk-stat-4",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
			{
				.type	= PCA9532_TYPE_NONE,
			},
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                	{
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
		},
	.pwm	= { 0, 0 },
	.psc	= { 0, 0 },
};

static struct i2c_board_info n5550_pca9532_0_info = {
	I2C_BOARD_INFO("pca9532", 0x64),
	.platform_data		= &n5550_pca9532_0_pdata,
};

static struct pca9532_platform_data n5550_pca9532_1_pdata = {
        .leds   = {
                        {
                                .type   = PCA9532_TYPE_GPIO,
				.state	= PCA9532_OFF,
                        },
                        {
                                .type   = PCA9532_TYPE_GPIO,
				.state	= PCA9532_OFF,
                        },
                        {
                                .type   = PCA9532_TYPE_GPIO,
				.state	= PCA9532_OFF,
                        },
                        {
                                .type   = PCA9532_TYPE_GPIO,
				.state	= PCA9532_OFF,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .name   = "n5550:orange:busy",
                                .type   = PCA9532_TYPE_LED,
                                .state  = PCA9532_OFF,
                        },
			{
				.name	= "n5550:blue:usb",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
			{
				.name	= "n5550:red:fail",
				.type	= PCA9532_TYPE_LED,
				.state	= PCA9532_OFF,
			},
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_NONE,
                        },
                        {
                                .type   = PCA9532_TYPE_GPIO,
				.state	= PCA9532_OFF,
                        },
		},
        .pwm    	= { 0, 0 },
        .psc    	= { 0, 0 },
	.gpio_base	= N5550_PCA9532_1_GPIO_BASE,
};

static struct i2c_board_info n5550_pca9532_1_info = {
        I2C_BOARD_INFO("pca9532", 0x62),
        .platform_data          = &n5550_pca9532_1_pdata,
};

#if 0
static struct gpio n5550_board_id_gpios[] = {
	{
		.gpio	= N5550_BOARD_ID_GPIO_0,
		.flags	= GPIOF_IN,
	},
	{
		.gpio	= N5550_BOARD_ID_GPIO_1,
		.flags	= GPIOF_IN,
	},
	{
		.gpio	= N5550_BOARD_ID_GPIO_2,
		.flags	= GPIOF_IN,
	},
	{
		.gpio	= N5550_BOARD_ID_GPIO_3,
		.flags	= GPIOF_IN,
	},
};
#endif

static struct i2c_client *n5550_pca9532_0_client, *n5550_pca9532_1_client;

static int __init n5550_pca9532_setup(void)
{
	struct i2c_adapter *adapter;
	struct pci_dev *dev;

	dev = pci_get_device(N5550_ICH_PCI_VENDOR, N5550_ICH_I2C_PCI_DEV, NULL);
	if (dev == NULL)
	    return -ENODEV;

	adapter = pci_get_drvdata(dev);
	if (adapter == NULL) {
		pci_dev_put(dev);
		return -ENODEV;
	}

	if (!try_module_get(adapter->owner)) {
		pci_dev_put(dev);
		return -EBUSY;
	}

	n5550_pca9532_0_client = i2c_new_device(adapter, &n5550_pca9532_0_info);
	if (n5550_pca9532_0_client == NULL) {
		module_put(adapter->owner);
		pci_dev_put(dev);
		return -ENODEV;
	}

	n5550_pca9532_1_client = i2c_new_device(adapter, &n5550_pca9532_1_info);
	if (n5550_pca9532_1_client == NULL) {
		i2c_unregister_device(n5550_pca9532_0_client);
		module_put(adapter->owner);
		pci_dev_put(dev);
		return -ENODEV;
	}

	module_put(adapter->owner);
	pci_dev_put(dev);
	return 0;
}

static void n5550_pca9532_cleanup(void)
{
	i2c_unregister_device(n5550_pca9532_0_client);
	i2c_unregister_device(n5550_pca9532_1_client);
}

#if 0
static int __init n5550_check_board_id(void)
{
	int id, ret;

	ret = gpio_request_array(n5550_board_id_gpios,
				 ARRAY_SIZE(n5550_board_id_gpios));
	if (ret != 0)
		return ret;

	id = (gpio_get_value_cansleep(N5550_BOARD_ID_GPIO_0) != 0);
	id |= (gpio_get_value_cansleep(N5550_BOARD_ID_GPIO_1) != 0) << 1;
	id |= (gpio_get_value_cansleep(N5550_BOARD_ID_GPIO_2) != 0) << 2;
	id |= (gpio_get_value_cansleep(N5550_BOARD_ID_GPIO_3) != 0) << 3;

	gpio_free_array(n5550_board_id_gpios, ARRAY_SIZE(n5550_board_id_gpios));

	if (id != N5550_BOARD_ID) {
		pr_warn("Incorrect Thecus board ID (%d)\n", id);
		return -ENODEV;
	}

	return 0;
}
#endif

static int __init n5550_board_init(void)
{
	int ret;

	ret = n5550_pca9532_setup();
	if (ret != 0) {
		pr_warn("n5550_pca9532_setup failed (%d)\n", ret);
		return ret;
	}
#if 0
	ret = n5550_check_board_id();
	if (ret != 0) {
		pr_warn("n5550_check_board_id failed (%d)\n", ret);
		goto error;
	}
#endif
	ret = n5550_ich_gpio_setup();
	if (ret != 0) {
		pr_warn("n5550_ich_gpio_setup failed (%d)\n", ret);
		goto error;
	}

	ret = n5550_ich_gpio_led_setup();
	if (ret != 0) {
		pr_warn("n5550_ich_gpio_led_setup failed (%d)\n", ret);
		goto error;
	}

	return 0;

error:
	n5550_pca9532_cleanup();
	return ret;
}

static void __exit n5550_board_exit(void)
{
	n5550_pca9532_cleanup();
	n5550_ich_gpio_led_cleanup();
}

module_init(n5550_board_init);
module_exit(n5550_board_exit);

MODULE_AUTHOR("Ian Pilcher <arequipeno@gmail.com>");
MODULE_DESCRIPTION("Thecus N5550 GPIO and LED support");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("dmi:bvnPhoenixTechnologiesLtd*:bvrCDV_T??X64:*:pnMilsteadPlatform:*:rnGraniteWell:rvrFABA:*:ct9:*");

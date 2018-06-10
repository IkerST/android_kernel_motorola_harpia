/*
 * Copyright (C) 2012-2013 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/qpnp/power-on.h>
#include <linux/delay.h>
#include <soc/qcom/watchdog.h>


enum mmi_factory_device_list {
	HONEYFUFU = 0,
	KUNGPOW,
};

#define KP_KILL_INDEX 0
#define KP_CABLE_INDEX 1
#define KP_WARN_INDEX 2
#define KP_NUM_GPIOS 3

struct mmi_factory_info {
	int num_gpios;
	struct gpio *list;
	int factory_cable;
	enum mmi_factory_device_list dev;
	struct delayed_work warn_irq_work;
	struct delayed_work fac_cbl_irq_work;
	int warn_irq;
	int fac_cbl_irq;
};


static int usr_rst_sw_dis_flg = -EINVAL;
static ssize_t usr_rst_sw_dis_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("Invalid value = %lu\n", mode);
		return -EINVAL;
	}

	usr_rst_sw_dis_flg = mode;

	return count;
}

static ssize_t usr_rst_sw_dis_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (usr_rst_sw_dis_flg > 0) ? "DISABLED" : "ENABLED");
}

static DEVICE_ATTR(usr_rst_sw_dis, 0664,
		   usr_rst_sw_dis_show,
		   usr_rst_sw_dis_store);

static int fac_kill_sw_dis_flg = -EINVAL;
static ssize_t fac_kill_sw_dis_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long r;
	unsigned long mode;

	r = kstrtoul(buf, 0, &mode);
	if (r) {
		pr_err("Invalid value = %lu\n", mode);
		return -EINVAL;
	}

	fac_kill_sw_dis_flg = mode;

	return count;
}

static ssize_t fac_kill_sw_dis_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 (fac_kill_sw_dis_flg > 0) ? "DISABLED" : "ENABLED");
}

static DEVICE_ATTR(fac_kill_sw_dis, 0664,
		   fac_kill_sw_dis_show,
		   fac_kill_sw_dis_store);

/* The driver should only be present when booting with the environment variable
 * indicating factory-cable is present.
 */
static bool mmi_factory_cable_present(void)
{
	struct device_node *np = of_find_node_by_path("/chosen");
	u32 fact_cable = 0;

	if (np)
		of_property_read_u32(np, "mmi,factory-cable", &fact_cable);

	of_node_put(np);
	return !!fact_cable ? true : false;
}

static int is_secure;
int __init secure_hardware_init(char *s)
{
	is_secure = !strncmp(s, "1", 1);

	return 1;
}
__setup("androidboot.secure_hardware=", secure_hardware_init);

static void warn_irq_w(struct work_struct *w)
{
	struct mmi_factory_info *info = container_of(w,
						     struct mmi_factory_info,
						     warn_irq_work.work);
	int warn_line = gpio_get_value(info->list[KP_WARN_INDEX].gpio);
	int reset_info = RESET_EXTRA_RESET_KUNPOW_REASON;

	if (!warn_line) {
		pr_info("HW User Reset!\n");
		pr_info("2 sec to Reset.\n");

#ifdef CONFIG_QPNP_POWER_ON
		/* trigger wdog if resin key pressed */
		if (qpnp_pon_key_status & QPNP_PON_KEY_RESIN_BIT && !is_secure) {
			pr_info("User triggered watchdog reset(Pwr + VolDn)\n");
			msm_trigger_wdog_bite();
			return;
		}
#endif
		/* Configure hardware reset before halt
		 * The new KUNGKOW circuit will not disconnect the battery if
		 * usb/dc is connected. But because the kernel is halted, a
		 * watchdog reset will be reported instead of hardware reset.
		 * In this case, we need to clear the KUNPOW reset bit to let
		 * BL detect it as a hardware reset.
		 * A pmic hard reset is necessary to report the powerup reason
		 * to BL correctly.
		 */
		if (usr_rst_sw_dis_flg <= 0) {
			qpnp_pon_store_extra_reset_info(reset_info, 0);
			qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);
			kernel_halt();
		} else
			pr_info("SW HALT Disabled!\n");

		return;
	}
}

#define WARN_IRQ_DELAY	5 /* 5msec */
static irqreturn_t warn_irq_handler(int irq, void *data)
{
	struct mmi_factory_info *info = data;

	/*schedule delayed work for 5msec for line state to settle*/
	queue_delayed_work(system_nrt_wq, &info->warn_irq_work,
			   msecs_to_jiffies(WARN_IRQ_DELAY));

	return IRQ_HANDLED;
}

static void fac_cbl_irq_w(struct work_struct *w)
{
	struct mmi_factory_info *info = container_of(w,
						     struct mmi_factory_info,
						     fac_cbl_irq_work.work);
	int fac_cbl_line = gpio_get_value(info->list[KP_CABLE_INDEX].gpio);
	int fac_cbl_kill_line = gpio_get_value(info->list[KP_KILL_INDEX].gpio);

	if (fac_cbl_line) {
		pr_info("Factory Cable Attached!\n");
		info->factory_cable = 1;
	} else
		if (info->factory_cable) {
			pr_info("Factory Cable Detached!\n");
			if (fac_cbl_kill_line) {
				info->factory_cable = 0;
				pr_info("Factory Kill Disabled!\n");
			} else {
				pr_info("2 sec to power off.\n");
				if (fac_kill_sw_dis_flg <= 0)
					kernel_power_off();
				else
					pr_info("SW POFF Disabled!\n");
				return;
			}
		}
}

#define FAC_CBL_IRQ_DELAY	5 /* 5msec */
static irqreturn_t fac_cbl_irq_handler(int irq, void *data)
{
	struct mmi_factory_info *info = data;

	/*schedule delayed work for 5msec for line state to settle*/
	queue_delayed_work(system_nrt_wq, &info->fac_cbl_irq_work,
			   msecs_to_jiffies(FAC_CBL_IRQ_DELAY));

	return IRQ_HANDLED;
}

static struct mmi_factory_info *mmi_parse_of(struct platform_device *pdev)
{
	struct mmi_factory_info *info;
	int gpio_count;
	struct device_node *np = pdev->dev.of_node;
	int i;
	enum of_gpio_flags flags;

	if (!np) {
		dev_err(&pdev->dev, "No OF DT node found.\n");
		return NULL;
	}

	gpio_count = of_gpio_count(np);

	if (!gpio_count) {
		dev_err(&pdev->dev, "No GPIOS defined.\n");
		return NULL;
	}

	/* Make sure number of GPIOs defined matches the supplied number of
	 * GPIO name strings.
	 */
	if (gpio_count != of_property_count_strings(np, "gpio-names")) {
		dev_err(&pdev->dev, "GPIO info and name mismatch\n");
		return NULL;
	}

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;

	info->list = devm_kzalloc(&pdev->dev,
				sizeof(struct gpio) * gpio_count,
				GFP_KERNEL);
	if (!info->list)
		return NULL;

	info->num_gpios = gpio_count;
	for (i = 0; i < gpio_count; i++) {
		info->list[i].gpio = of_get_gpio_flags(np, i, &flags);
		info->list[i].flags = flags;
		of_property_read_string_index(np, "gpio-names", i,
						&info->list[i].label);
		dev_dbg(&pdev->dev, "GPIO: %d  FLAGS: %ld  LABEL: %s\n",
			info->list[i].gpio, info->list[i].flags,
			info->list[i].label);
	}

	return info;
}

static enum mmi_factory_device_list hff_dev = HONEYFUFU;
static enum mmi_factory_device_list kp_dev = KUNGPOW;

static const struct of_device_id mmi_factory_of_tbl[] = {
	{ .compatible = "mmi,factory-support-msm8960", .data = &hff_dev},
	{ .compatible = "mmi,factory-support-kungpow", .data = &kp_dev},
	{},
};
MODULE_DEVICE_TABLE(of, mmi_factory_of_tbl);

static int mmi_factory_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct mmi_factory_info *info;
	int ret;
	int i, warn_line;

	match = of_match_device(mmi_factory_of_tbl, &pdev->dev);
	if (!match) {
		dev_err(&pdev->dev, "No Match found\n");
		return -ENODEV;
	}

	if (match && match->compatible)
		dev_info(&pdev->dev, "Using %s\n", match->compatible);

	info = mmi_parse_of(pdev);
	if (!info) {
		dev_err(&pdev->dev, "failed to parse node\n");
		return -ENODEV;
	}

	ret = gpio_request_array(info->list, info->num_gpios);
	if (ret) {
		dev_err(&pdev->dev, "failed to request GPIOs\n");
		return ret;
	}

	for (i = 0; i < info->num_gpios; i++) {
		ret = gpio_export(info->list[i].gpio, 1);
		if (ret) {
			dev_err(&pdev->dev, "Failed to export GPIO %s: %d\n",
				info->list[i].label, info->list[i].gpio);
			goto fail;
		}

		ret = gpio_export_link(&pdev->dev, info->list[i].label,
					info->list[i].gpio);
		if (ret) {
			dev_err(&pdev->dev, "Failed to link GPIO %s: %d\n",
				info->list[i].label, info->list[i].gpio);
			goto fail;
		}
	}

	if (!mmi_factory_cable_present()) {
		dev_dbg(&pdev->dev, "factory cable not present\n");
	} else {
		pr_info("Factory Cable Attached at Power up!\n");
		info->factory_cable = 1;
	}

	if (match && match->data) {
		info->dev = *(enum mmi_factory_device_list *)(match->data);
	} else {
		dev_err(&pdev->dev, "failed to find device match\n");
		goto fail;
	}

	/* Toggle factory kill disable line */
	warn_line = gpio_get_value(info->list[KP_WARN_INDEX].gpio);

	if (!warn_line && !info->factory_cable) {
		gpio_direction_output(info->list[KP_KILL_INDEX].gpio, 1);
		udelay(50);
		gpio_direction_output(info->list[KP_KILL_INDEX].gpio, 0);
		udelay(50);
		gpio_direction_output(info->list[KP_KILL_INDEX].gpio, 1);
		udelay(50);
		gpio_direction_output(info->list[KP_KILL_INDEX].gpio, 0);
		udelay(50);
		gpio_direction_output(info->list[KP_KILL_INDEX].gpio, 1);
	}

	if ((info->dev == KUNGPOW) && (info->num_gpios == KP_NUM_GPIOS)) {
		/* Disable Kill if not powered up by a factory cable */
		if (!info->factory_cable)
			gpio_direction_output(info->list[KP_KILL_INDEX].gpio,
						1);
		else {
			ret = device_create_file(&pdev->dev,
						 &dev_attr_usr_rst_sw_dis);
			if (ret)
				dev_err(&pdev->dev,
					"couldn't create usr_rst_sw_dis\n");

			usr_rst_sw_dis_flg = 0;

			ret = device_create_file(&pdev->dev,
						&dev_attr_fac_kill_sw_dis);
			if (ret)
				dev_err(&pdev->dev,
					"couldn't create fac_kill_sw_dis\n");

			fac_kill_sw_dis_flg = 0;
		}

		info->warn_irq = gpio_to_irq(info->list[KP_WARN_INDEX].gpio);
		info->fac_cbl_irq =
			gpio_to_irq(info->list[KP_CABLE_INDEX].gpio);

		INIT_DELAYED_WORK(&info->warn_irq_work, warn_irq_w);
		INIT_DELAYED_WORK(&info->fac_cbl_irq_work, fac_cbl_irq_w);

		if (info->warn_irq) {
			ret = request_irq(info->warn_irq,
					  warn_irq_handler,
					  IRQF_TRIGGER_FALLING,
					  "mmi_factory_warn", info);
			if (ret) {
				dev_err(&pdev->dev,
					"request irq failed for Warn\n");
				goto fail;
			}
		} else {
			ret = -ENODEV;
			dev_err(&pdev->dev, "IRQ for Warn doesn't exist\n");
			goto fail;
		}

		if (info->fac_cbl_irq) {
			ret = request_irq(info->fac_cbl_irq,
					  fac_cbl_irq_handler,
					  IRQF_TRIGGER_RISING |
					  IRQF_TRIGGER_FALLING,
					  "mmi_factory_fac_cbl", info);
			if (ret) {
				dev_err(&pdev->dev,
					"irq failed for Factory Cable\n");
				goto remove_warn;
			}
		} else {
			ret = -ENODEV;
			dev_err(&pdev->dev,
				"IRQ for Factory Cable doesn't exist\n");
			goto remove_warn;
		}
	}

	platform_set_drvdata(pdev, info);

	return 0;

remove_warn:
	free_irq(info->warn_irq, info);
fail:
	gpio_free_array(info->list, info->num_gpios);
	return ret;
}

static int mmi_factory_remove(struct platform_device *pdev)
{
	struct mmi_factory_info *info = platform_get_drvdata(pdev);

	if (usr_rst_sw_dis_flg >= 0)
		device_remove_file(&pdev->dev,
				   &dev_attr_usr_rst_sw_dis);
	if (fac_kill_sw_dis_flg >= 0)
		device_remove_file(&pdev->dev,
				   &dev_attr_fac_kill_sw_dis);
	if (info) {
		gpio_free_array(info->list, info->num_gpios);

		cancel_delayed_work_sync(&info->warn_irq_work);
		cancel_delayed_work_sync(&info->fac_cbl_irq_work);
		if (info->fac_cbl_irq)
			free_irq(info->fac_cbl_irq, info);
		if (info->warn_irq)
			free_irq(info->warn_irq, info);
	}

	return 0;
}

static struct platform_driver mmi_factory_driver = {
	.probe = mmi_factory_probe,
	.remove = mmi_factory_remove,
	.driver = {
		.name = "mmi_factory",
		.owner = THIS_MODULE,
		.of_match_table = mmi_factory_of_tbl,
	},
};

static int __init mmi_factory_init(void)
{
	return platform_driver_register(&mmi_factory_driver);
}

static void __exit mmi_factory_exit(void)
{
	platform_driver_unregister(&mmi_factory_driver);
}

module_init(mmi_factory_init);
module_exit(mmi_factory_exit);

MODULE_ALIAS("platform:mmi_factory");
MODULE_AUTHOR("Motorola Mobility LLC");
MODULE_DESCRIPTION("Motorola Mobility Factory Specific Controls");
MODULE_LICENSE("GPL");

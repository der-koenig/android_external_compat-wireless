/*
 * Copyright (c) 2012 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <mach/msm_xo.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include "debug.h"
#include "platform.h"

static struct ath6kl_reg_data ar6004_reg_table[] = {
	{"qca,ar6004-vbatt", 3140000, 3460000, 720000, 0, true, NULL},
	{"qca,ar6004-vdd-io", 1710000, 3460000, 30000, 5, true, NULL}
};

#define MAX_PROP_SIZE 32
static int ath6kl_dt_parse_vreg_info(struct device *dev,
		struct ath6kl_reg_data *reg_table, u32 size)
{
	char prop_name[MAX_PROP_SIZE];
	struct device_node *np = dev->of_node;
	int i;

	for (i = 0; i < size; i++) {
		snprintf(prop_name, MAX_PROP_SIZE, "%s-supply",
			reg_table[i].name);

		/* Check if this regulator required. If the property does not
		 * exist then this regulator is not required. It is sourced
		 * directly from Battery.
		 */
		if (of_get_property(np, prop_name, NULL) == NULL) {
			ath6kl_dbg(ATH6KL_DBG_PLAT,
				"%s: No regulator required for %s\n",
					__func__, reg_table[i].name);
			reg_table[i].is_required = false;
		}
	}

	return 0;
}

static int ath6kl_dt_parse_gpio_info(struct device *dev,
	struct ath6kl_gpio_data *gpio_data)
{
	int gpio_no;
	struct device_node *np = dev->of_node;

	if ((gpio_no = of_get_named_gpio(np, "qca,chip-pwd-l-gpios", 0)) < 0) {
		ath6kl_err("Please specify the CHIP_PWD_L GPIO in platform device tree\n");
		return -EINVAL;
	}

	ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: chip-pwd-l-gpio = %d\n",
		__func__, gpio_no);
	gpio_data->chip_pwd_l_gpio = gpio_no;


	if ((gpio_no = of_get_named_gpio(np, "qca,pm-enable-gpios", 0)) < 0) {
		ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: pm-enable-gpio is not provided",
			__func__, gpio_no);
		gpio_data->pm_enable_gpio = -1;
	} else {
		ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: pm-enable-gpio = %d\n",
			__func__, gpio_no);
		gpio_data->pm_enable_gpio = gpio_no;
	}

	if ((gpio_no = of_get_named_gpio(np, "qca,wlan-clk-req-gpios", 0)) < 0) {
		ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: wlan-clk-req-gpio is not provided",
			__func__, gpio_no);
		gpio_data->wlan_clk_req_gpio = -1;
	} else {
		ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: wlan-clk-req-gpio = %d\n",
			__func__, gpio_no);
		gpio_data->wlan_clk_req_gpio = gpio_no;
	}

	return 0;
}

static struct ath6kl_platform_data *ath6kl_dt_populate_pdata(
	struct device *dev)
{
	struct ath6kl_platform_data *pdata = NULL;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);

	if (!pdata) {
		ath6kl_err("%s: Could not allocate memory for platform data\n",
			__func__);
		goto err;
	}

	/* For now AR6004 is the only chip supported. For multiple chip
	 * support, extra property can be added to decide type of hardware. */
	pdata->reg_table = ar6004_reg_table;
	pdata->reg_table_size = ARRAY_SIZE(ar6004_reg_table);

	if (ath6kl_dt_parse_vreg_info(dev, pdata->reg_table,
				pdata->reg_table_size) != 0) {
		goto err;
	}

	if (ath6kl_dt_parse_gpio_info(dev, &pdata->gpio_data) != 0) {
		goto err;
	}

	return pdata;

err:
	if (pdata != NULL)
		devm_kfree(dev, pdata);
	return NULL;
}

static void ath6kl_vregs_off(struct device *dev,
		struct ath6kl_reg_data *reg_table, u32 size)
{
	int rc = 0;
	int i;

	if (!dev) {
		ath6kl_err("%s: device point is NULL\n", __func__);
		return;
	}

	if (!reg_table || size == 0) {
		ath6kl_err("%s: Invalid regulator table reg_table: %p, size: %d\n",
				__func__, reg_table, size);
		return;
	}

	for (i = size - 1; i >= 0; i--) {

		if (!reg_table[i].is_required) {
			continue;
		}

		if (!reg_table[i].reg) {
			ath6kl_err("%s: Reg is NULL!!!\n", __func__);
			continue;
		}

		rc = regulator_disable(reg_table[i].reg);

		rc = regulator_set_voltage(reg_table[i].reg, 0,
			reg_table[i].high_vol_level);

		rc = regulator_set_optimum_mode(reg_table[i].reg, 0);

		regulator_put(reg_table[i].reg);
		reg_table[i].reg = NULL;
	}
}

static int ath6kl_vregs_on(struct device *dev,
		struct ath6kl_reg_data *reg_table, u32 size)
{
	int rc = 0;
	int i = 0;

	if (!dev) {
		ath6kl_err("%s: Device point is NULL\n", __func__);
		rc = -ENODEV;
		goto err;
	}

	if (!reg_table || size == 0) {
		ath6kl_err("%s: Invalid regulator table reg_table: %p, size: %d\n",
				__func__, reg_table, size);
		rc = -EINVAL;
		goto err;
	}

	for (i = 0; i < size; i++) {

		if (!reg_table[i].is_required) {
			ath6kl_dbg(ATH6KL_DBG_PLAT, "%s: Not required to enable %s regulator",
					__func__, reg_table[i].name);
			continue;
		}

		reg_table[i].reg = regulator_get(dev, reg_table[i].name);

		if (!reg_table[i].reg || IS_ERR(reg_table[i].reg)) {
			rc = PTR_ERR(reg_table[i].reg);
			ath6kl_err("Failed to get regulator: %s, rc: %d\n",
					reg_table[i].name, rc);
			reg_table[i].reg = NULL;
			goto err;
		}

		rc = regulator_set_voltage(reg_table[i].reg,
			reg_table[i].low_vol_level,
			reg_table[i].high_vol_level);

		if (rc) {
			ath6kl_err("Failed to set regulator voltage: %s, rc: %d\n",
					reg_table[i].name, rc);
			goto err;
		}

		rc = regulator_set_optimum_mode(reg_table[i].reg,
				reg_table[i].load_uA);

		if (rc < 0) {
			ath6kl_err("Failed to set regulator optimum mode: %s, rc: %d\n",
					reg_table[i].name, rc);
			goto err;
		}

		rc = regulator_enable(reg_table[i].reg);
		if (rc) {
			ath6kl_err("Failed to enable regulator: %s, rc: %d\n",
					reg_table[i].name, rc);
			goto err;
		}

		mdelay(reg_table[i].delay_mT);
	}

	return rc;

err:
	ath6kl_vregs_off(dev, reg_table, i + 1);
	return rc;
}

static int ath6kl_gpio_helper(int gpio, int on, const char *name)
{
	int rc = 0;

	if (on) {
		rc = gpio_request(gpio, name);

		if (rc) {
			ath6kl_err("%s: gpio_request failed, gpio: %d, name: %s\n",
					__func__, gpio, name);
			return rc;
		}

		if ((rc = gpio_direction_output(gpio, on))) {
			gpio_free(gpio);
			return rc;
		}
	} else {
		gpio_direction_input(gpio);
		gpio_free(gpio);
	}

	return rc;
}

static int ath6kl_platform_power(struct ath6kl_platform_data *pdata, int on)
{
	int rc = 0;

	ath6kl_info("%s: %d\n", __func__, on);

	if (on) {
		rc = ath6kl_vregs_on(&pdata->pdev->dev, pdata->reg_table,
				pdata->reg_table_size);

		if (rc) {
			ath6kl_err("%s: Regulator setup failed\n", __func__);
			goto reg_fail;
		}

		if (pdata->gpio_data.pm_enable_gpio >= 0) {
			rc = ath6kl_gpio_helper(pdata->gpio_data.pm_enable_gpio,
				on, "ath6kl-pm-enable");

			if (rc) {
				goto gpio_fail;
			}
		}

		mdelay(5);

		rc = ath6kl_gpio_helper(pdata->gpio_data.chip_pwd_l_gpio, on,
			"ath6kl-chip-pwd-l");

		if (rc) {
			goto gpio_fail;
		}

		ath6kl_info("Power-up done successfully!!!\n");

	} else {
		ath6kl_gpio_helper(pdata->gpio_data.chip_pwd_l_gpio, on, NULL);
		ath6kl_gpio_helper(pdata->gpio_data.pm_enable_gpio, on, NULL);
		ath6kl_vregs_off(&pdata->pdev->dev, pdata->reg_table,
			pdata->reg_table_size);
	}

	return rc;

gpio_fail:
	ath6kl_gpio_helper(pdata->gpio_data.chip_pwd_l_gpio, on, NULL);
	ath6kl_gpio_helper(pdata->gpio_data.pm_enable_gpio, on, NULL);

reg_fail:
	return rc;
}

static int ath6kl_platform_probe(struct platform_device *pdev)
{
	struct ath6kl_platform_data *pdata = NULL;

	if (pdev->dev.of_node) {
		pdata = ath6kl_dt_populate_pdata(&pdev->dev);
	}

	if (!pdata) {
		return -EINVAL;
	}

	pdata->pdev = pdev;

	platform_set_drvdata(pdev, pdata);

	return ath6kl_platform_power(pdata, 1);

}

static int ath6kl_platform_remove(struct platform_device *pdev)
{
	struct ath6kl_platform_data *pdata = platform_get_drvdata(pdev);

	ath6kl_platform_power(pdata, 0);
	return 0;
}

static int ath6kl_platform_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	return 0;
}

static int ath6kl_platform_resume(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ath6kl_dt_match[] = {
	{.compatible = "qca,ar6004-sdio"},
	{.compatible = "qca,ar6004-hsic"},
	{},
};

MODULE_DEVICE_TABLE(of, ath6kl_dt_match);

static struct platform_driver ath6kl_driver = {
	.probe		= ath6kl_platform_probe,
	.remove		= ath6kl_platform_remove,
	.suspend	= ath6kl_platform_suspend,
	.resume		= ath6kl_platform_resume,
	.driver		= {
		.name = "wlan-ath6kl-platform",
		.of_match_table = ath6kl_dt_match,
	},
};

int ath6kl_platform_driver_register(void) {

	return platform_driver_register(&ath6kl_driver);
}

EXPORT_SYMBOL(ath6kl_platform_driver_register);

void ath6kl_platform_driver_unregister(void) {

	platform_driver_unregister(&ath6kl_driver);
}

EXPORT_SYMBOL(ath6kl_platform_driver_unregister);


#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <mach/msm_xo.h>
#include <linux/platform_device.h>
struct platform_device *g_pdev = NULL;

extern void msm7x27a_wifi_power(bool on);
//#define plat_setup_power(ar, on, detect) msm7x27a_wifi_power(on) 
#define MSM9615_WLAN_CHIP_PWD_PIN               21
#define MSM9615_WLAN_PM_ENABLE_PIN              89 /* PM8018_GPIO_BASE + PM GPIO_2 - 1 */

#ifdef ATH_AR6K_EXT_PMIC_CLK
#define MSM9615_WLAN_CLK_PWR_REQ                93 /* PM8018_GPIO_BASE + PM GPIO_6 - 1 */
#endif /* ATH_AR6K_EXT_PMIC_CLK */

#define A_MDELAY(msecs)	mdelay(msecs)
//#define plat_setup_power(ar, on, detect) msm9615_wifi_power(ar, on)

struct wlan_regulator {
    const char *vreg_name; /* Regulator Name */
    int min_uV; /* Minimum voltage at which AR6003 can operate */
    int max_uV; /* Maximum voltage at which AR6003 can operate */
    int load_uA; /* Current which will be drawn from regulator (Worst case) */
    int delay_mT; /* Time from this operation to next */
    struct regulator *vreg; /* Regulator Handle */
};


static struct wlan_regulator regulator_table[] = {
    {"wlan_vreg", 1710000, 1890000, 86000, 5, NULL}
};

//#ifdef ATH_AR6K_EXT_PMIC_CLK
//static struct msm_xo_voter *xo_handle = NULL;
//#endif

static void msm9615_wifi_power_down(struct wlan_regulator *regulators, u32 size)
{
    int rc = 0;
    int i;

    if (!regulators) {
        printk("msm9615_wifi_power_down: NULL pointer passed!!!\n");
        return;
    }

    for (i = size - 1; i >= 0; i--) {

        if (!regulators[i].vreg) {
            printk("msm9615_wifi_power_down: vreg is NULL!!!\n");
            continue;
        }

        rc = regulator_disable(regulators[i].vreg);

        if (rc) {
            printk("Failed to disable regulator: %s, rc: %d\n",
                        regulators[i].vreg_name, rc);
        }

        rc = regulator_set_voltage(regulators[i].vreg, 0, regulators[i].max_uV);

        if (rc) {
            printk("Failed to set regulator voltage: %s, rc: %d\n",
                        regulators[i].vreg_name, rc);
        }

        rc = regulator_set_optimum_mode(regulators[i].vreg, 0);

        if (rc < 0) {
            printk("Failed to set regulator optimum mode: %s, rc: %d\n",
                        regulators[i].vreg_name, rc);
        }

        regulator_put(regulators[i].vreg);
        regulators[i].vreg = NULL;
    }
}

static int msm9615_wifi_power_up(struct wlan_regulator *regulators, u32 size)
{
    int rc = 0;
    int i = 0;

    if (!g_pdev) {
        printk ("msm9615_wifi_power_up: Platform device is NULL!!!\n");
        rc = -ENODEV;
        goto fail;
    }

    if (!regulators) {
        printk("msm9615_wifi_power_up: NULL pointer passed!!!");
        rc = -EINVAL;
        goto fail;
    }

    for (i = 0; i < size; i++) {

        regulators[i].vreg = regulator_get(&g_pdev->dev, regulators[i].vreg_name);

        if (!regulators[i].vreg || IS_ERR(regulators[i].vreg)) {
            rc = PTR_ERR(regulators[i].vreg);
            //AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to get regulator: %s, rc: %d\n",
            //            regulators[i].vreg_name, rc));
            regulators[i].vreg = NULL;
            goto fail;
        }

        rc = regulator_set_voltage(regulators[i].vreg, regulators[i].min_uV, regulators[i].max_uV);

        if (rc) {
            //AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to set regulator voltage: %s, rc: %d\n",
            //            regulators[i].vreg_name, rc));
            goto fail;
        }

        rc = regulator_set_optimum_mode(regulators[i].vreg, regulators[i].load_uA);

        if (rc < 0) {
            //AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to set regulator optimum mode: %s, rc: %d\n",
            //            regulators[i].vreg_name, rc));
            goto fail;
        }

        rc = regulator_enable(regulators[i].vreg);
        if (rc) {
            //AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to enable regulator: %s, rc: %d\n",
            //            regulators[i].vreg_name, rc));
            goto fail;
        }

        A_MDELAY(regulators[i].delay_mT);
    }

    return rc;

fail:
    msm9615_wifi_power_down(regulators, i + 1);
    return rc;
}

static struct gpio wifi_gpios[] = {
    { MSM9615_WLAN_PM_ENABLE_PIN, GPIOF_OUT_INIT_LOW, "wlan_pm_enable" },
    { MSM9615_WLAN_CHIP_PWD_PIN, GPIOF_OUT_INIT_LOW, "wlan_chip_pwd_l" },
#ifdef ATH_AR6K_EXT_PMIC_CLK
    { MSM9615_WLAN_CLK_PWR_REQ, GPIOF_OUT_INIT_LOW, "wlan_clk_pwr_req" },
#endif /* ATH_AR6K_EXT_PMIC_CLK */
};

int msm9615_wifi_power(int on)
{
    int rc = 0;

   printk("msm9615_wifi_power: %s\n", on ? "on" : "off");

    if (on) {
        rc = msm9615_wifi_power_up(regulator_table, ARRAY_SIZE(regulator_table));

        if (rc) {
//            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("msm9615_wifi_power: Power UP failed!!!\n"));
            goto power_up_fail;
        }

        rc = gpio_request_array(wifi_gpios, ARRAY_SIZE(wifi_gpios));

        if (rc) {
//            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Cannot request GPIO!!!"));
            goto gpio_request_fail;
        }

#ifdef ATH_AR6K_EXT_PMIC_CLK
        xo_handle = msm_xo_get(MSM_XO_TCXO_A1, "wlan");

        if (IS_ERR(xo_handle)) {
            rc = PTR_ERR(xo_handle);
//            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to get handle to vote for TCXO A1 buffer: %d\n", rc));

            goto xo_get_fail;
        }
#endif /* ATH_AR6K_EXT_PMIC_CLK */

        gpio_set_value(MSM9615_WLAN_PM_ENABLE_PIN, 1);
        A_MDELAY(100);
        gpio_set_value(MSM9615_WLAN_CHIP_PWD_PIN, 1);

#ifdef ATH_AR6K_EXT_PMIC_CLK
        gpio_set_value(MSM9615_WLAN_CLK_PWR_REQ, 1);

        rc = msm_xo_mode_vote(xo_handle, MSM_XO_MODE_ON);

        if (rc) {
//            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Failed to vote for TCXO A1 buffer: %d\n", rc));
            goto xo_mode_vote_fail;
        }
#endif /* ATH_AR6K_EXT_PMIC_CLK */

    } else {

#ifdef ATH_AR6K_EXT_PMIC_CLK
        /* msm_xo_put also removes the vote */
        msm_xo_put(xo_handle);
        xo_handle = NULL;
        gpio_set_value(MSM9615_WLAN_CLK_PWR_REQ, 0);
#endif
        gpio_set_value(MSM9615_WLAN_CHIP_PWD_PIN, 0);
        gpio_set_value(MSM9615_WLAN_PM_ENABLE_PIN, 0);

        gpio_free_array(wifi_gpios, ARRAY_SIZE(wifi_gpios));

        msm9615_wifi_power_down(regulator_table, ARRAY_SIZE(regulator_table));
    }

    return rc;

#ifdef ATH_AR6K_EXT_PMIC_CLK
xo_mode_vote_fail:
    msm_xo_put(xo_handle);
    xo_handle = NULL;

    gpio_set_value(MSM9615_WLAN_CLK_PWR_REQ, 0);
    gpio_set_value(MSM9615_WLAN_CHIP_PWD_PIN, 0);
    gpio_set_value(MSM9615_WLAN_PM_ENABLE_PIN, 0);

xo_get_fail:
    gpio_free_array(wifi_gpios, ARRAY_SIZE(wifi_gpios));
#endif /* ATH_AR6K_EXT_PMIC_CLK */

gpio_request_fail:
    msm9615_wifi_power_down(regulator_table, ARRAY_SIZE(regulator_table));

power_up_fail:
    return rc;
}

static int ar6000_pm_probe(struct platform_device *pdev)
{

    g_pdev = pdev;

    //plat_setup_power(NULL, 1, 1);
    msm9615_wifi_power(1);
    return 0;
}

static int ar6000_pm_remove(struct platform_device *pdev)
{
    //plat_setup_power(NULL, 0, 1);
    msm9615_wifi_power(0);
    return 0;
}

static int ar6000_pm_suspend(struct platform_device *pdev, pm_message_t state)
{
    return 0;
}

static int ar6000_pm_resume(struct platform_device *pdev)
{
     return 0;
}
struct platform_driver ar6000_pm_device = {
    .probe      = ar6000_pm_probe,
    .remove     = ar6000_pm_remove,
    .suspend    = ar6000_pm_suspend,
    .resume     = ar6000_pm_resume,
    .driver     = {
        .name = "wlan_ar6000_pm_dev",
    },
};

void ath6kl_platform_driver_register(void){
     platform_driver_register(&ar6000_pm_device);
}

void ath6kl_platform_driver_unregister(void){
     platform_driver_unregister(&ar6000_pm_device);
}
//#endif /* defined(CONFIG_MMC_MSM) && defined(CONFIG_ARCH_MSM9615) */

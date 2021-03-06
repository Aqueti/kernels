/*
 * tegra_usb_cd.c -- Tegra USB charger detection driver.
 *
 * Copyright (c) 2012-2017, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <soc/tegra/chip-id.h>

#include "tegra_usb_cd.h"

static bool vbus_wakelock = true;
static DEFINE_SPINLOCK(wl_spinlock);
static struct vbus_lock lock;

static void vbus_hold_wl(struct vbus_lock *lock)
{
	if (!lock->held) {
		wake_lock(&lock->wakelock);
		lock->held = true;
		pr_debug("[hold VBUS wakelock]\n");
	}
}

#define TEMPORARY_WAKELOCK_HOLD_TIME	2000
static void vbus_hold_temp_wl(struct vbus_lock *lock)
{
	wake_lock_timeout(&lock->wakelock,
			  msecs_to_jiffies(TEMPORARY_WAKELOCK_HOLD_TIME));
	lock->held = false;
	pr_debug("[hold temporary VBUS wakelock for %d ms]\n",
		 TEMPORARY_WAKELOCK_HOLD_TIME);
}

static void vbus_drop_wl(struct vbus_lock *lock)
{
	if (lock->held) {
		wake_unlock(&lock->wakelock);
		lock->held = false;
		pr_debug("[drop VBUS wakelock]\n");
	}
}

static const unsigned int tegra_usb_cd_extcon_cable[] = {
	EXTCON_USB, /* USB */
	EXTCON_CHG_USB_DCP, /* TA */
	EXTCON_USB_QC2, /* QC2 */
	EXTCON_USB_MAXIM, /* MAXIM */
	EXTCON_CHG_USB_CDP, /* Charge-downstream */
	EXTCON_CHG_USB_SLOW, /* Slow-charger */
	EXTCON_USB_APPLE_500mA, /* Apple 500mA-charger */
	EXTCON_USB_APPLE_1A, /* Apple 1A-charger */
	EXTCON_USB_APPLE_2A, /* Apple 2A-charger */
	EXTCON_NONE,
};

static void tegra_usb_cd_update_wakelock(struct tegra_usb_cd *ucd)
{
	unsigned long flags;

	if (!vbus_wakelock)
		return;

	spin_lock_irqsave(&wl_spinlock, flags);

	switch (ucd->connect_type) {
	case EXTCON_USB:
	case EXTCON_CHG_USB_CDP:
		vbus_hold_wl(&lock);
		break;
	case EXTCON_NONE:
		vbus_drop_wl(&lock);
		break;
	default:
		vbus_hold_temp_wl(&lock);
	};

	spin_unlock_irqrestore(&wl_spinlock, flags);
}

static void tegra_usb_cd_update_charging_extcon_state(struct tegra_usb_cd *ucd)
{
	struct extcon_dev *edev = ucd->edev;
	u32 old_state, new_state;

	if (edev == NULL)
		return;

	old_state = extcon_get_state(edev);
	extcon_set_state(edev, 0x0);
	extcon_set_cable_state_(edev, ucd->connect_type, true);
	new_state = extcon_get_state(edev);
	dev_info(ucd->dev, "notification status (0x%x, 0x%x)\n",
				old_state, new_state);
}

static int tegra_usb_cd_set_current_limit(struct tegra_usb_cd *ucd, int max_ua)
{
	int ret = 0;

	if (max_ua > 0 && ucd->hw_ops->vbus_pad_protection)
		ucd->hw_ops->vbus_pad_protection(ucd, true);

	dev_info(ucd->dev, "set current %dma\n", max_ua/1000);
	if (ucd->vbus_reg != NULL)
		ret = regulator_set_current_limit(ucd->vbus_reg, 0, max_ua);

	if (max_ua == 0 && ucd->hw_ops->vbus_pad_protection)
		ucd->hw_ops->vbus_pad_protection(ucd, false);

	return ret;
}

static int tegra_usb_cd_update_charging_current(struct tegra_usb_cd *ucd)
{
	int max_ua = 0, ret = 0;

	switch (ucd->connect_type) {
	case EXTCON_NONE:
		dev_info(ucd->dev, "disconnected USB cable/charger\n");
		ucd->sdp_cdp_current_limit_ma = 0;
		max_ua = 0;
		break;
	case EXTCON_USB:
		if (ucd->sdp_cdp_current_limit_ma > 2)
			dev_info(ucd->dev, "connected to SDP\n");
		max_ua = min(ucd->sdp_cdp_current_limit_ma * 1000,
				USB_CHARGING_SDP_CURRENT_LIMIT_UA);
		break;
	case EXTCON_CHG_USB_DCP:
		dev_info(ucd->dev, "connected to DCP\n");
		max_ua = max(ucd->dcp_current_limit_ma * 1000,
				USB_CHARGING_DCP_CURRENT_LIMIT_UA);
		break;
	case EXTCON_USB_QC2:
		dev_info(ucd->dev, "connected to QuickCharge 2(wall charger)\n");
		max_ua = ucd->qc2_current_limit_ma * 1000;
		break;
	case EXTCON_USB_MAXIM:
		dev_info(ucd->dev, "connected to Maxim(wall charger)\n");
		max_ua = ucd->dcp_current_limit_ma * 1000;
		break;
	case EXTCON_CHG_USB_CDP:
		dev_info(ucd->dev, "connected to CDP\n");
		if (ucd->sdp_cdp_current_limit_ma > 2)
			max_ua = USB_CHARGING_CDP_CURRENT_LIMIT_UA;
		else
			max_ua = ucd->sdp_cdp_current_limit_ma * 1000;
		break;
	case EXTCON_CHG_USB_SLOW:
		dev_info(ucd->dev, "connected to non-standard charger\n");
		max_ua = USB_CHARGING_NON_STANDARD_CHARGER_CURRENT_LIMIT_UA;
		break;
	case EXTCON_USB_APPLE_500mA:
		dev_info(ucd->dev, "connected to Apple/Other 0.5A custom charger\n");
		max_ua = USB_CHARGING_APPLE_CHARGER_500mA_CURRENT_LIMIT_UA;
		break;
	case EXTCON_USB_APPLE_1A:
		dev_info(ucd->dev, "connected to Apple/Other 1A custom charger\n");
		max_ua = USB_CHARGING_APPLE_CHARGER_1000mA_CURRENT_LIMIT_UA;
		break;
	case EXTCON_USB_APPLE_2A:
		dev_info(ucd->dev, "connected to Apple/Other/NV 2A custom charger\n");
		max_ua = USB_CHARGING_APPLE_CHARGER_2000mA_CURRENT_LIMIT_UA;
		break;
	default:
		dev_info(ucd->dev, "connected to unknown USB port\n");
		max_ua = 0;
	}

	ucd->current_limit_ma = max_ua/1000;
	ret = tegra_usb_cd_set_current_limit(ucd, max_ua);

	return ret;
}

static unsigned int
	tegra_usb_cd_detect_cable_and_set_current(struct tegra_usb_cd *ucd)
{
	int ret;

	if (ucd->hw_ops == NULL)
		return ucd->connect_type;

	ucd->hw_ops->power_on(ucd);

	if (ucd->hw_ops->apple_cd) {
		ret = ucd->hw_ops->apple_cd(ucd);
		switch(ret) {
		case APPLE_500MA:
			ucd->connect_type = EXTCON_USB_APPLE_500mA;
			goto power_off;
                        break;
		case APPLE_1000MA:
			ucd->connect_type = EXTCON_USB_APPLE_1A;
			goto power_off;
                        break;
		case APPLE_2000MA:
			ucd->connect_type = EXTCON_USB_APPLE_2A;
			goto power_off;
                        break;
		default:
			dev_dbg(ucd->dev, "Not an apple charger\n");
		}
	}

	if (ucd->hw_ops->dcp_cd && ucd->hw_ops->dcp_cd(ucd)) {
		/*
		 * wait 20ms (max of TVDMSRC_DIS) for D- to be disabled
		 * from host side, before we perform secondary detection.
		 * Some hosts may not respond well if we do secondary
		 * detection right after primary detection.
		 */
		msleep(20);
		if (ucd->hw_ops->cdp_cd && ucd->hw_ops->cdp_cd(ucd))
			ucd->connect_type = EXTCON_CHG_USB_CDP;
		else if (ucd->hw_ops->maxim14675_cd &&
				ucd->hw_ops->maxim14675_cd(ucd))
			ucd->connect_type = EXTCON_USB_MAXIM;
		else if (ucd->qc2_voltage) {
			ucd->connect_type = EXTCON_CHG_USB_SLOW;
			tegra_usb_cd_update_charging_current(ucd);
			if (ucd->hw_ops->qc2_cd &&
				ucd->hw_ops->qc2_cd(ucd))
				ucd->connect_type = EXTCON_USB_QC2;
			else
				ucd->connect_type = EXTCON_CHG_USB_DCP;
		} else
			ucd->connect_type = EXTCON_CHG_USB_DCP;
	} else
		ucd->connect_type = EXTCON_USB;

power_off:
	ucd->hw_ops->power_off(ucd);

	tegra_usb_cd_update_charging_extcon_state(ucd);
	tegra_usb_cd_update_charging_current(ucd);
	tegra_usb_cd_update_wakelock(ucd);

	return ucd->connect_type;
}

/* --------------------------- */
/* API's for controller driver */

void tegra_ucd_set_charger_type(struct tegra_usb_cd *ucd,
				unsigned int connect_type)
{
	if (ucd == NULL)
		return;

	ucd->connect_type = connect_type;
	tegra_usb_cd_update_charging_extcon_state(ucd);
	tegra_usb_cd_update_charging_current(ucd);
	tegra_usb_cd_update_wakelock(ucd);
}
EXPORT_SYMBOL_GPL(tegra_ucd_set_charger_type);

unsigned int
	tegra_ucd_detect_cable_and_set_current(struct tegra_usb_cd *ucd)
{
	if (IS_ERR(ucd)) {
		dev_err(ucd->dev, "ucd not initialized");
		return -EINVAL;
	}

	return tegra_usb_cd_detect_cable_and_set_current(ucd);
}
EXPORT_SYMBOL_GPL(tegra_ucd_detect_cable_and_set_current);

struct tegra_usb_cd *tegra_usb_get_ucd(struct platform_device *pdev)
{
	struct tegra_usb_cd *ucd;

	if (!pdev)
		return ERR_PTR(-EINVAL);

	ucd = platform_get_drvdata(pdev);

	if (!ucd)
		return ERR_PTR(-EINVAL);

	ucd->open_count++;

	return ucd;
}
EXPORT_SYMBOL_GPL(tegra_usb_get_ucd);

void tegra_usb_release_ucd(struct tegra_usb_cd *ucd)
{
	if (ucd == NULL)
		return;

	ucd->open_count--;
}
EXPORT_SYMBOL_GPL(tegra_usb_release_ucd);

void tegra_ucd_set_current(struct tegra_usb_cd *ucd, int current_ma)
{
	if (ucd == NULL)
		return;

	ucd->current_limit_ma = current_ma;
	tegra_usb_cd_set_current_limit(ucd, current_ma*1000);
}
EXPORT_SYMBOL_GPL(tegra_ucd_set_current);

void tegra_ucd_set_sdp_cdp_current(struct tegra_usb_cd *ucd, int current_ma)
{
	if (ucd == NULL)
		return;

	if (ucd->connect_type != EXTCON_USB
			&& ucd->connect_type != EXTCON_CHG_USB_CDP) {
		dev_err(ucd->dev,
			"%s: ignore the request to set %dmA for non SDP/CDP port",
			__func__, current_ma);
		return;
	}

	ucd->sdp_cdp_current_limit_ma = current_ma;
	tegra_usb_cd_update_charging_current(ucd);
}
EXPORT_SYMBOL_GPL(tegra_ucd_set_sdp_cdp_current);

static struct tegra_usb_cd_soc_data tegra186_soc_config = {
	.init_hw_ops = tegra18x_usb_cd_init_ops,
};

static struct tegra_usb_cd_soc_data tegra210_soc_config = {
	.init_hw_ops = tegra21x_usb_cd_init_ops,
};

static const struct of_device_id tegra_usb_cd_of_match[] = {
	{.compatible = "nvidia,tegra186-usb-cd", .data = &tegra186_soc_config,},
	{.compatible = "nvidia,tegra210-usb-cd", .data = &tegra210_soc_config,},
	{},
};
MODULE_DEVICE_TABLE(of, tegra_usb_cd_of_match);

static void tegra_usb_cd_parse_dt(struct platform_device *pdev,
					struct tegra_usb_cd *ucd)
{
	struct device_node *np = pdev->dev.of_node;
	u32 current_ua = 0;

	if (!np)
		return;

	of_property_read_u32(np, "nvidia,dcp-current-limit-ua", &current_ua);
	ucd->dcp_current_limit_ma = current_ua / 1000;
	current_ua = 0;
	of_property_read_u32(np, "nvidia,qc2-current-limit-ua", &current_ua);
	ucd->qc2_current_limit_ma = current_ua / 1000;
	of_property_read_u32(np, "nvidia,qc2-input-voltage",
					&ucd->qc2_voltage);
}

static int tegra_usb_cd_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct tegra_usb_cd_soc_data *soc_data;
	struct tegra_usb_cd *ucd;
	struct regulator *vbus_reg;
	int err = 0;

	match = of_match_device(of_match_ptr(tegra_usb_cd_of_match),
			&pdev->dev);

	if (!match)
		return -ENODEV;

	ucd = devm_kzalloc(&pdev->dev, sizeof(struct tegra_usb_cd), GFP_KERNEL);
	if (!ucd)
		return -ENOMEM;

	ucd->phy = devm_phy_get(&pdev->dev, "otg-phy");
	if (IS_ERR(ucd->phy)) {
		if (PTR_ERR(ucd->phy) != -EPROBE_DEFER) {
			dev_err(&pdev->dev, "failed to get otg port phy %ld\n",
				PTR_ERR(ucd->phy));
		} else
			dev_info(&pdev->dev, "otg phy is not available yet\n");

		return PTR_ERR(ucd->phy);
	}

	ucd->dev = &pdev->dev;
	ucd->connect_type = EXTCON_NONE;
	ucd->current_limit_ma = 0;
	ucd->sdp_cdp_current_limit_ma = 0;
	ucd->open_count = 0;
	soc_data = (struct tegra_usb_cd_soc_data *)match->data;
	platform_set_drvdata(pdev, ucd);

	tegra_usb_cd_parse_dt(pdev, ucd);

	/* Prepare and register extcon device for charging notification */
	ucd->edev = devm_extcon_dev_allocate(&pdev->dev,
					     tegra_usb_cd_extcon_cable);
	if (IS_ERR(ucd->edev)) {
		dev_err(&pdev->dev, "failed to allocate extcon device\n");
		return PTR_ERR(ucd->edev);
	}

	err = devm_extcon_dev_register(&pdev->dev, ucd->edev);
	if (err) {
		dev_err(&pdev->dev, "failed to register extcon device\n");
		return err;
	}

	/* Get the vbus current regulator for charging */
	vbus_reg = devm_regulator_get_optional(&pdev->dev, "usb_bat_chg");
	if (!IS_ERR(vbus_reg)) {
		dev_info(&pdev->dev, "USB charging enabled\n");
		ucd->vbus_reg = vbus_reg;
	}

	/* setup HW ops */
	soc_data->init_hw_ops(ucd);
	if (ucd->hw_ops == NULL || ucd->hw_ops->open == NULL) {
		dev_err(ucd->dev, "no charger detection hw_ops found\n");
		return err;
	}

	err = ucd->hw_ops->open(ucd);
	if (err) {
		dev_err(ucd->dev, "hw_ops open failed\n");
		return err;
	}

	wake_lock_init(&lock.wakelock, WAKE_LOCK_SUSPEND, "vbus-wakelock");

	return 0;
}

static int tegra_usb_cd_remove(struct platform_device *pdev)
{
	struct tegra_usb_cd *ucd = platform_get_drvdata(pdev);

	if (!ucd)
		return -ENODEV;

	if (ucd->open_count)
		return -EBUSY;

	if (ucd->hw_ops != NULL && ucd->hw_ops->close)
		ucd->hw_ops->close(ucd);

	wake_lock_destroy(&lock.wakelock);

	return 0;
}

static struct platform_driver tegra_usb_cd_driver = {
	.driver = {
		.name = "tegra-usb-cd",
		.owner = THIS_MODULE,
		.of_match_table = tegra_usb_cd_of_match,
	},
	.probe = tegra_usb_cd_probe,
	.remove = tegra_usb_cd_remove,
};

static int __init tegra_usb_cd_init(void)
{
	return platform_driver_register(&tegra_usb_cd_driver);
}

static void __exit tegra_usb_cd_exit(void)
{
	platform_driver_unregister(&tegra_usb_cd_driver);
}

device_initcall(tegra_usb_cd_init);
module_exit(tegra_usb_cd_exit);

static int set_vbus_wakelock(const char *val, const struct kernel_param *kp)
{
	int rv = param_set_bool(val, kp);
	unsigned long flags;

	if (rv)
		return rv;

	/* release wakelock if held */
	if (!*(bool *)kp->arg) {
		spin_lock_irqsave(&wl_spinlock, flags);
		vbus_drop_wl(&lock);
		spin_unlock_irqrestore(&wl_spinlock, flags);
	}

	return 0;
}

static struct kernel_param_ops vbus_wakelock_param_ops = {
	.set = set_vbus_wakelock,
	.get = param_get_bool,
};

module_param_cb(vbus_wakelock, &vbus_wakelock_param_ops, &vbus_wakelock,
		S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(vbus_wakelock,
		 "enable wakelock when detected as USB downstream port");

MODULE_DESCRIPTION("Tegra USB charger detection driver");
MODULE_AUTHOR("Rakesh Babu Bodla <rbodla@nvidia.com>");
MODULE_LICENSE("GPL v2");

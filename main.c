/*
 * mac80211 glue code for mac80211 Silicon Labs WFX drivers
 *
 * Copyright (c) 2017, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
 * Copyright (c) 2008, Johannes Berg <johannes@sipsolutions.net>
 * Copyright (c) 2008 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (c) 2007-2009, Christian Lamparter <chunkeey@web.de>
 * Copyright (c) 2006, Michael Wu <flamingice@sourmilk.net>
 * Copyright (c) 2004-2006 Jean-Baptiste Note <jbnote@gmail.com>, et al.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/mmc/sdio_func.h>
#include <linux/spi/spi.h>
#include <linux/etherdevice.h>

#include "wfx_version.h"
#include "wfx_api.h"
#include "wfx.h"
#include "fwio.h"
#include "hwio.h"
#include "bh.h"
#include "sta.h"
#include "debug.h"
#include "wsm.h"
#include "testmode.h"

MODULE_DESCRIPTION("Silicon Labs 802.11 Wireless LAN driver for WFx");
MODULE_LICENSE("GPL");
MODULE_VERSION(WFX_LABEL);
// Legacy
MODULE_ALIAS("wfx-core");
MODULE_ALIAS("wfx-wlan-spi");
MODULE_ALIAS("wfx-wlan-sdio");

static int gpio_wakeup = -2;
module_param(gpio_wakeup, int, 0644);
MODULE_PARM_DESC(gpio_wakeup, "gpio number for wakeup. -1 for none.");

int power_mode = -1;
module_param(power_mode, int, 0644);
MODULE_PARM_DESC(power_mode, "Force power save mode. 0: Disabled, 1: Allow doze, 2: Allow quiescent");

#define wfx_a_rates        (wfx_rates + 4)
#define wfx_a_rates_size    (ARRAY_SIZE(wfx_rates) - 4)
#define wfx_g_rates        (wfx_rates + 0)
#define wfx_g_rates_size    (ARRAY_SIZE(wfx_rates))
#define wfx_n_rates        (wfx_mcs_rates)
#define wfx_n_rates_size    (ARRAY_SIZE(wfx_mcs_rates))

#define RATETAB_ENT(_rate, _rateid, _flags)		\
	{						\
		.bitrate	= (_rate),		\
		.hw_value	= (_rateid),		\
		.flags		= (_flags),		\
	}

#define CHAN2G(_channel, _freq, _flags) {            \
		.band = NL80211_BAND_2GHZ,        \
		.center_freq = (_freq),            \
		.hw_value = (_channel),            \
		.flags = (_flags),            \
		.max_antenna_gain = 0,                \
		.max_power = 30,                \
}


static struct ieee80211_rate wfx_rates[] = {
	RATETAB_ENT(10,  0,   0),
	RATETAB_ENT(20,  1,   0),
	RATETAB_ENT(55,  2,   0),
	RATETAB_ENT(110, 3,   0),
	RATETAB_ENT(60,  6,  0),
	RATETAB_ENT(90,  7,  0),
	RATETAB_ENT(120, 8,  0),
	RATETAB_ENT(180, 9,  0),
	RATETAB_ENT(240, 10, 0),
	RATETAB_ENT(360, 11, 0),
	RATETAB_ENT(480, 12, 0),
	RATETAB_ENT(540, 13, 0),
};

static struct ieee80211_rate wfx_mcs_rates[] = {
	RATETAB_ENT(65,  14, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(130, 15, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(195, 16, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(260, 17, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(390, 18, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(520, 19, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(585, 20, IEEE80211_TX_RC_MCS),
	RATETAB_ENT(650, 21, IEEE80211_TX_RC_MCS),
};

static struct ieee80211_channel wfx_2ghz_chantable[] = {
	CHAN2G(1, 2412, 0),
	CHAN2G(2, 2417, 0),
	CHAN2G(3, 2422, 0),
	CHAN2G(4, 2427, 0),
	CHAN2G(5, 2432, 0),
	CHAN2G(6, 2437, 0),
	CHAN2G(7, 2442, 0),
	CHAN2G(8, 2447, 0),
	CHAN2G(9, 2452, 0),
	CHAN2G(10, 2457, 0),
	CHAN2G(11, 2462, 0),
	CHAN2G(12, 2467, 0),
	CHAN2G(13, 2472, 0),
	CHAN2G(14, 2484, 0),
};


static struct ieee80211_supported_band wfx_band_2ghz = {
	.channels = wfx_2ghz_chantable,
	.n_channels = ARRAY_SIZE(wfx_2ghz_chantable),
	.bitrates = wfx_g_rates,
	.n_bitrates = wfx_g_rates_size,

	/**
	 * LDPC: WFx driver supports only Transmit LDPC Tx.
	 * IEEE80211_HT_CAP_LDPC_CODING should not be enabled
	 *
	 * SGI: WFx driver supports only Short GI for 20 MHZ
	 * IEEE80211_HT_CAP_SGI_40 capability should not be enabled
	 *
	 *
	 */
	.ht_cap = {
		.cap			= IEEE80211_HT_CAP_GRN_FLD | /* Receive Greenfield */
					  IEEE80211_HT_CAP_SGI_20 |  /* Receive Short GI for 20MHZ */
					  (1 <<
		IEEE80211_HT_CAP_RX_STBC_SHIFT) |                    /* Receive STBC for 20MHZ */
			IEEE80211_HT_CAP_MAX_AMSDU,
		.ht_supported = 1,
		.ampdu_factor		= IEEE80211_HT_MAX_AMPDU_16K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
		.mcs = {
			.rx_mask[0] = 0xFF,
			.rx_highest = cpu_to_le16(0x41),
			.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
		},
	},
};


static const unsigned long wfx_ttl[] = {
	1 * HZ,	/* VO */
	2 * HZ,	/* VI */
	5 * HZ, /* BE */
	10 * HZ	/* BK */
};

static const struct ieee80211_ops wfx_ops = {
	.start			= wfx_start,
	.stop			= wfx_stop,
#ifdef CONFIG_PM /* CONFIG_PM */
	.suspend		= wfx_wow_suspend,
	.resume			= wfx_wow_resume,
#endif /* CONFIG_PM */
	.add_interface		= wfx_add_interface,
	.change_interface	= wfx_change_interface,
	.remove_interface	= wfx_remove_interface,
	.config			= wfx_config,
	.tx			= wfx_tx,
	.conf_tx		= wfx_conf_tx,
	.hw_scan		= wfx_hw_scan,
	.sta_add		= wfx_sta_add,
	.sta_remove		= wfx_sta_remove,
	.sta_notify		= wfx_sta_notify,
	.set_tim		= wfx_set_tim,
	.set_key		= wfx_set_key,
	.set_rts_threshold	= wfx_set_rts_threshold,
	.bss_info_changed	= wfx_bss_info_changed,
	.prepare_multicast	= wfx_prepare_multicast,
	.configure_filter	= wfx_configure_filter,
	.get_stats		= wfx_get_stats,
	.ampdu_action		= wfx_ampdu_action,
	.flush			= wfx_flush,
#ifdef CONFIG_NL80211_TESTMODE
	.testmode_cmd		= wfx_testmode_command,
#endif
};

#ifdef CONFIG_PM
static const struct wiphy_wowlan_support wfx_wowlan_support = {
	/* Support only for limited wowlan functionalities */
	.flags = WIPHY_WOWLAN_ANY | WIPHY_WOWLAN_DISCONNECT,
};

#endif

struct gpio_desc *wfx_get_gpio(struct device *dev, int override, const char *label)
{
	struct gpio_desc *ret;
	char label_buf[256];

	if (override >= 0) {
		snprintf(label_buf, sizeof(label_buf), "wfx_%s", label);
		ret = ERR_PTR(devm_gpio_request_one(dev, override, GPIOF_OUT_INIT_LOW, label_buf));
		if (!ret)
			ret = gpio_to_desc(override);
	} else if (override == -1) {
		ret = NULL;
	} else {
		ret = devm_gpiod_get(dev, label, GPIOD_OUT_LOW);
	}
	if (IS_ERR(ret) || !ret) {
		if (!ret || PTR_ERR(ret) == -ENOENT)
			dev_warn(dev, "gpio %s is not defined", label);
		else
			dev_warn(dev, "error while requesting gpio %s", label);
		ret = NULL;
	} else {
		dev_dbg(dev, "using gpio %d for %s", desc_to_gpio(ret), label);
	}
	return ret;
}

static struct ieee80211_hw *wfx_init_common(const struct wfx_platform_data *pdata, struct device *dev)
{
	int i, band;
	struct ieee80211_hw *hw;
	struct wfx_dev *wdev;

	hw = ieee80211_alloc_hw(sizeof(struct wfx_dev), &wfx_ops);
	if (!hw)
		return NULL;
	SET_IEEE80211_DEV(hw, dev);

	wdev = hw->priv;
	wdev->hw = hw;
	wdev->hw_type = -1;
	wdev->pdev = dev;
	memcpy(&wdev->pdata, pdata, sizeof(*pdata));
	of_property_read_string(dev->of_node, "config-file", &wdev->pdata.file_pds);
	if (power_mode >= 0 && power_mode <= 2)
		wdev->pdata.power_mode = power_mode;
	wdev->pdata.gpio_wakeup = wfx_get_gpio(dev, gpio_wakeup, "wakeup");
	if (!wdev->pdata.gpio_wakeup && wdev->pdata.power_mode == WSM_OP_POWER_MODE_QUIESCENT) {
		wdev->pdata.power_mode = WSM_OP_POWER_MODE_DOZE;
		dev_warn(wdev->pdev, "disable WSM_OP_POWER_MODE_QUIESCENT");
	}

	wdev->rates = wfx_rates;
	wdev->mcs_rates = wfx_n_rates;

	ieee80211_hw_set(hw, NEED_DTIM_BEFORE_ASSOC);
	ieee80211_hw_set(hw, TX_AMPDU_SETUP_IN_HW);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	ieee80211_hw_set(hw, CONNECTION_MONITOR);
	ieee80211_hw_set(hw, REPORTS_TX_ACK_STATUS);
	ieee80211_hw_set(hw, SUPPORTS_DYNAMIC_PS);
	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, SUPPORTS_PS);
	ieee80211_hw_set(hw, MFP_CAPABLE);

	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
					  BIT(NL80211_IFTYPE_ADHOC) |
					  BIT(NL80211_IFTYPE_AP) |
					  BIT(NL80211_IFTYPE_MESH_POINT) |
					  BIT(NL80211_IFTYPE_P2P_CLIENT) |
					  BIT(NL80211_IFTYPE_P2P_GO);

#ifdef CONFIG_PM
	hw->wiphy->wowlan = &wfx_wowlan_support;
#endif

	hw->wiphy->flags |= WIPHY_FLAG_AP_UAPSD;

	hw->queues = 4;

	hw->max_rates = 8;
	hw->max_rate_tries = 15;
	hw->extra_tx_headroom = WSM_TX_EXTRA_HEADROOM +
		8;  /* TKIP IV */

	hw->vif_data_size = sizeof(struct wfx_vif);
	hw->sta_data_size = sizeof(struct wfx_sta_priv);

	hw->wiphy->bands[NL80211_BAND_2GHZ] = &wfx_band_2ghz;

	/* Channel params have to be cleared before registering wiphy again */
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *sband = hw->wiphy->bands[band];

		if (!sband)
			continue;
		for (i = 0; i < sband->n_channels; i++) {
			sband->channels[i].flags = 0;
			sband->channels[i].max_antenna_gain = 0;
			sband->channels[i].max_power = 30;
		}
	}

	hw->wiphy->max_scan_ssids = 2;
	hw->wiphy->max_scan_ie_len = IEEE80211_MAX_DATA_LEN;

	mutex_init(&wdev->wsm_cmd_mux);
	mutex_init(&wdev->conf_mutex);
	wdev->workqueue = create_singlethread_workqueue("wfx_wq");
	sema_init(&wdev->scan.lock, 1);
	INIT_WORK(&wdev->scan.work, wfx_scan_work);
	INIT_DELAYED_WORK(&wdev->scan.probe_work, wfx_probe_work);
	INIT_DELAYED_WORK(&wdev->scan.timeout, wfx_scan_timeout);

	INIT_WORK(&wdev->tx_policy_upload_work, tx_policy_upload_work);
	if (wfx_queue_stats_init(&wdev->tx_queue_stats, WFX_LINK_ID_MAX,
				 wfx_skb_dtor, wdev)) {
		goto err1;
	}

	for (i = 0; i < 4; ++i) {
		if (wfx_queue_init(&wdev->tx_queue[i], &wdev->tx_queue_stats,
				   i, 16, wfx_ttl[i])) {
			for (; i > 0; i--)
				wfx_queue_deinit(&wdev->tx_queue[i - 1]);
			goto err2;
		}
	}

	init_waitqueue_head(&wdev->channel_switch_done);
	init_waitqueue_head(&wdev->wsm_cmd_wq);
	init_waitqueue_head(&wdev->wsm_startup_done);
	init_waitqueue_head(&wdev->ps_mode_switch_done);
	wsm_buf_init(&wdev->wsm_cmd_buf);
	spin_lock_init(&wdev->wsm_cmd.lock);
	wdev->wsm_cmd.done = 1;
	tx_policy_init(wdev);

	return hw;
err2:
	wfx_queue_stats_deinit(&wdev->tx_queue_stats);
err1:
	ieee80211_free_hw(hw);
	return NULL;
}

static int wfx_register_common(struct ieee80211_hw *dev)
{
	struct wfx_dev *wdev = dev->priv;
	int ret;

	ret = ieee80211_register_hw(dev);
	if (ret)
		goto err1;

	ret = wfx_debug_init(wdev);
	if (ret)
		goto err2;

#ifdef CONFIG_PM
	ret = wfx_pm_init(&wdev->pm_state, wdev);
	if (ret)
		goto err2;
#endif

	return ret;

err2:
	ieee80211_unregister_hw(dev);
err1:
	return ret;
}

static void wfx_free_common(struct ieee80211_hw *dev)
{
	struct wfx_dev *wdev = dev->priv;

	if (wdev->pdata.gpio_wakeup)
		gpiod_set_value(wdev->pdata.gpio_wakeup, 0);
	ieee80211_free_hw(dev);
}

static void wfx_unregister_common(struct ieee80211_hw *dev)
{
	struct wfx_dev *wdev = dev->priv;
	int i;

	ieee80211_unregister_hw(dev);

	wfx_unregister_bh(wdev);

	mutex_destroy(&wdev->conf_mutex);

	wsm_buf_deinit(&wdev->wsm_cmd_buf);

	destroy_workqueue(wdev->workqueue);
	wdev->workqueue = NULL;

	for (i = 0; i < 4; ++i)
		wfx_queue_deinit(&wdev->tx_queue[i]);

	wfx_queue_stats_deinit(&wdev->tx_queue_stats);
#ifdef CONFIG_PM
	wfx_pm_deinit(&wdev->pm_state);
#endif
}

int wfx_core_probe(const struct wfx_platform_data *pdata,
		   const struct hwbus_ops *hwbus_ops,
		      struct hwbus_priv *hwbus,
		      struct device *pdev,
		   struct wfx_dev **core)
{
	int err = -EINVAL;
	struct ieee80211_hw *dev;
	struct wfx_dev *wdev;
	struct wsm_operational_mode mode = { };
	const void *macaddr;

	dev = wfx_init_common(pdata, pdev);
	if (!dev)
		goto err;

	wdev = dev->priv;
	wdev->hwbus_ops = hwbus_ops;
	wdev->hwbus_priv = hwbus;

	/* Pass struct wfx_dev back up */
	*core = wdev;

	err = wfx_register_bh(wdev);
	if (err)
		goto err1;

	err = wfx_init_device(wdev);
	if (err)
		goto err2;

	// LDPC support was not yet tested
	wdev->pdata.support_ldpc = false;

	if (wait_event_interruptible_timeout(wdev->wsm_startup_done,
					     wdev->firmware_ready,
					     10 * HZ) <= 0) {
		dev_err(wdev->pdev, "timeout while waiting for startup indication. IRQ configuration error?\n");
		err = -ETIMEDOUT;
		goto err2;
	}
	msleep(100);

	dev_dbg(wdev->pdev, "sending configuration file %s", wdev->pdata.file_pds);
	err = wfx_send_pdata_pds(wdev);
	if (err < 0)
		goto err2;

	if (wdev->pdata.power_mode == WSM_OP_POWER_MODE_QUIESCENT) {
		/* Driver must switch WUP gpio to 0 to allow sleep
		 * and set it to 1 to wakeup the device
		 * control_register WUP bit used to wakeup the device at reset
		 * must be set back to 0
		 * else sleep is not possible
		 */
		gpiod_set_value(wdev->pdata.gpio_wakeup, 1);
		control_reg_write(wdev, 0);
		wdev->sleep_activated = true;
		dev_dbg(wdev->pdev, "enable 'quiescent' power mode with gpio %d and PDS file %s\n",
			desc_to_gpio(wdev->pdata.gpio_wakeup), wdev->pdata.file_pds);
	}

	mode.disable_more_flag_usage = true;
	mode.power_mode = wdev->pdata.power_mode;
	wsm_set_operational_mode(wdev, &mode);

	wsm_use_multi_tx_conf(wdev, true);

	eth_zero_addr(dev->wiphy->perm_addr);
	macaddr = of_get_mac_address(pdev->of_node);
	if (macaddr)
		ether_addr_copy(dev->wiphy->perm_addr, macaddr);
	if (!is_valid_ether_addr(dev->wiphy->perm_addr))
		wsm_get_station_id(wdev, dev->wiphy->perm_addr, NULL);
	if (!is_valid_ether_addr(dev->wiphy->perm_addr)) {
		dev_warn(wdev->pdev, "using random ethernet MAC\n");
		eth_random_addr(dev->wiphy->perm_addr);
	}
	dev_info(wdev->pdev, "ethernet MAC: %pM\n", dev->wiphy->perm_addr);

	err = wfx_register_common(dev);
	if (err)
		goto err2;

	return err;

err2:
	wfx_unregister_bh(wdev);
err1:
	wfx_free_common(dev);
err:
	*core = NULL;
	return err;
}

void wfx_core_release(struct wfx_dev *self)
{
	wfx_unregister_common(self->hw);
	config_reg_write_bits(self, CFG_IRQ_ENABLE_DATA | CFG_IRQ_ENABLE_WRDY, 0);
	wfx_free_common(self->hw);
}

extern struct sdio_driver wfx_sdio_driver;
extern struct spi_driver wfx_spi_driver;
static int __init wfx_core_init(void)
{
	int ret = 0;

	pr_info("wfx: Silicon Labs " WFX_LABEL "\n");

	if (IS_ENABLED(CONFIG_SPI))
		ret = spi_register_driver(&wfx_spi_driver);
	if (IS_ENABLED(CONFIG_MMC) && !ret)
		ret = sdio_register_driver(&wfx_sdio_driver);
	return ret;
}
module_init(wfx_core_init);

static void __exit wfx_core_exit(void)
{
	if (IS_ENABLED(CONFIG_MMC))
		sdio_unregister_driver(&wfx_sdio_driver);
	if (IS_ENABLED(CONFIG_SPI))
		spi_unregister_driver(&wfx_spi_driver);
}
module_exit(wfx_core_exit);
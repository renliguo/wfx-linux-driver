/*
 * Mac80211 SDIO driver for Silicon Labs WFX device
 *
 * Copyright (c) 2017, Silicon Laboratories, Inc.
 * Copyright (c) 2010, ST-Ericsson
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
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>

#include "wfx.h"
#include "hwbus.h"
#include "main.h"
#include "bh.h"

static const struct wfx_platform_data wfx_sdio_pdata = {
	.file_fw = "wfm_wf200",
	.file_pds = "wf200.pds",
	.support_ldpc = true,
	.sdio = true,
};

struct wfx_sdio_priv {
	struct sdio_func *func;
	struct wfx_dev *core;
	u8 buf_id_tx;
	u8 buf_id_rx;
	int of_irq;
};

static int wfx_sdio_copy_from_io(void *priv, unsigned int reg_id,
				 void *dst, size_t count)
{
	struct wfx_sdio_priv *bus = priv;
	unsigned int sdio_addr = reg_id << 2;
	int ret;

	BUG_ON(reg_id > 7);
	WARN(((uintptr_t) dst) & 3, "Unaligned buffer size");
	WARN(count & 3, "Unaligned buffer address");

	/* Use queue mode buffers */
	if (reg_id == WFX_REG_IN_OUT_QUEUE)
		sdio_addr |= (bus->buf_id_rx + 1) << 7;
	ret = sdio_memcpy_fromio(bus->func, dst, sdio_addr, count);
	if (!ret && reg_id == WFX_REG_IN_OUT_QUEUE)
		bus->buf_id_rx = (bus->buf_id_rx + 1) % 4;

	return ret;
}

static int wfx_sdio_copy_to_io(void *priv, unsigned int reg_id,
			       const void *src, size_t count)
{
	struct wfx_sdio_priv *bus = priv;
	unsigned int sdio_addr = reg_id << 2;
	int ret;

	BUG_ON(reg_id > 7);
	WARN(((uintptr_t) src) & 3, "Unaligned buffer size");
	WARN(count & 3, "Unaligned buffer address");

	/* Use queue mode buffers */
	if (reg_id == WFX_REG_IN_OUT_QUEUE)
		sdio_addr |= bus->buf_id_tx << 7;
	// FIXME: discards 'const' qualifier for src
	ret = sdio_memcpy_toio(bus->func, sdio_addr, (void *) src, count);
	if (!ret && reg_id == WFX_REG_IN_OUT_QUEUE)
		bus->buf_id_tx = (bus->buf_id_tx + 1) % 32;

	return ret;
}

static void wfx_sdio_lock(void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	sdio_claim_host(bus->func);
}

static void wfx_sdio_unlock(void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	sdio_release_host(bus->func);
}

static void wfx_sdio_irq_handler(struct sdio_func *func)
{
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);

	if (bus->core)
		wfx_irq_handler(bus->core);
	else
		WARN(!bus->core, "race condition in driver init/deinit");
}

static irqreturn_t wfx_sdio_irq_handler_ext(int irq, void *priv)
{
	struct wfx_sdio_priv *bus = priv;

	if (!bus->core) {
		WARN(!bus->core, "race condition in driver init/deinit");
		return IRQ_NONE;
	}
	sdio_claim_host(bus->func);
	wfx_irq_handler(bus->core);
	sdio_release_host(bus->func);
	return IRQ_HANDLED;
}

static int wfx_sdio_irq_subscribe(struct wfx_sdio_priv *bus)
{
	int ret;

	if (bus->of_irq) {
		ret = request_irq(bus->of_irq, wfx_sdio_irq_handler_ext,
				  IRQF_TRIGGER_RISING, "wfx", bus);
	} else {
		sdio_claim_host(bus->func);
		ret = sdio_claim_irq(bus->func, wfx_sdio_irq_handler);
		sdio_release_host(bus->func);
	}
	return ret;
}

static int wfx_sdio_irq_unsubscribe(struct wfx_sdio_priv *bus)
{
	int ret;

	if (bus->of_irq) {
		free_irq(bus->of_irq, bus);
		ret = 0;
	} else {
		sdio_claim_host(bus->func);
		ret = sdio_release_irq(bus->func);
		sdio_release_host(bus->func);
	}
	return ret;
}

static size_t wfx_sdio_align_size(void *priv, size_t size)
{
	struct wfx_sdio_priv *bus = priv;

	return sdio_align_size(bus->func, size);
}

static const struct hwbus_ops wfx_sdio_hwbus_ops = {
	.copy_from_io = wfx_sdio_copy_from_io,
	.copy_to_io = wfx_sdio_copy_to_io,
	.lock			= wfx_sdio_lock,
	.unlock			= wfx_sdio_unlock,
	.align_size		= wfx_sdio_align_size,
};

static const struct of_device_id wfx_sdio_of_match[];
static int wfx_sdio_probe(struct sdio_func *func,
			  const struct sdio_device_id *id)
{
	struct device_node *np = func->dev.of_node;
	struct wfx_sdio_priv *bus;
	int ret;

	if (func->num != 1) {
		dev_err(&func->dev, "SDIO function number is %d while it should always be 1 (unsupported chip?)", func->num);
		return -ENODEV;
	}

	bus = devm_kzalloc(&func->dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	if (!np) {
		dev_warn(&func->dev, "Device is not declared in DT. Features will be limited.");
		// FIXME: ignore VID/PID and only rely on device tree
		// return -ENODEV;
	} else if (!of_match_node(wfx_sdio_of_match, np)) {
		dev_warn(&func->dev, "No compatible device found in DT");
		return -ENODEV;
	} else {
		bus->of_irq = irq_of_parse_and_map(np, 0);
	}

	bus->func = func;
	sdio_set_drvdata(func, bus);
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0 | MMC_QUIRK_BLKSZ_FOR_BYTE_MODE | MMC_QUIRK_BROKEN_BYTE_MODE_512;

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	// Block of 64 bytes is more efficient than 512B for frame sizes < 4k
	sdio_set_block_size(func, 64);
	sdio_release_host(func);
	if (ret)
		goto err0;

	ret = wfx_sdio_irq_subscribe(bus);
	if (ret)
		goto err1;

	bus->core = wfx_init_common(&func->dev, &wfx_sdio_pdata,
				    &wfx_sdio_hwbus_ops, bus);
	if (!bus->core) {
		ret = -EIO;
		goto err2;
	}

	ret = wfx_probe(bus->core);
	if (ret)
		goto err3;

	return 0;

err3:
	wfx_free_common(bus->core);
err2:
	wfx_sdio_irq_unsubscribe(bus);
err1:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err0:
	return ret;
}

static void wfx_sdio_remove(struct sdio_func *func)
{
	struct wfx_sdio_priv *bus = sdio_get_drvdata(func);

	wfx_release(bus->core);
	wfx_free_common(bus->core);
	wfx_sdio_irq_unsubscribe(bus);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
}

#define SDIO_VENDOR_ID_SILABS        0x0000
#define SDIO_DEVICE_ID_SILABS_WF200  0x1000
static const struct sdio_device_id wfx_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SILABS, SDIO_DEVICE_ID_SILABS_WF200) },
	// FIXME: ignore VID/PID and only rely on device tree
	// { SDIO_DEVICE(SDIO_ANY_ID, SDIO_ANY_ID) },
	{ },
};
MODULE_DEVICE_TABLE(sdio, wfx_sdio_ids);

#ifdef CONFIG_OF
static const struct of_device_id wfx_sdio_of_match[] = {
	{ .compatible = "silabs,wfx-sdio" },
	{ },
};
MODULE_DEVICE_TABLE(of, wfx_sdio_of_match);
#endif

struct sdio_driver wfx_sdio_driver = {
	.name = "wfx-sdio",
	.id_table = wfx_sdio_ids,
	.probe = wfx_sdio_probe,
	.remove = wfx_sdio_remove,
	.drv = {
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(wfx_sdio_of_match),
	}
};

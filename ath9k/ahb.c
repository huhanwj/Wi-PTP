/*
 * Copyright (c) 2008-2011 Atheros Communications Inc.
 * Copyright (c) 2009 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (c) 2009 Imre Kaloz <kaloz@openwrt.org>
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

#include <linux/nl80211.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>
#include "ath9k.h"
#include <linux/ath9k_platform.h>

#ifdef CONFIG_OF
#include <asm/mach-ath79/ath79.h>
#include <asm/mach-ath79/ar71xx_regs.h>
#include <linux/mtd/mtd.h>
#endif

static const struct platform_device_id ath9k_platform_id_table[] = {
	{
		.name = "ath9k",
		.driver_data = AR5416_AR9100_DEVID,
	},
	{
		.name = "ar933x_wmac",
		.driver_data = AR9300_DEVID_AR9330,
	},
	{
		.name = "ar934x_wmac",
		.driver_data = AR9300_DEVID_AR9340,
	},
	{
		.name = "qca955x_wmac",
		.driver_data = AR9300_DEVID_QCA955X,
	},
	{
		.name = "qca953x_wmac",
		.driver_data = AR9300_DEVID_AR953X,
	},
	{
		.name = "qca956x_wmac",
		.driver_data = AR9300_DEVID_QCA956X,
	},
	{},
};

/* return bus cachesize in 4B word units */
static void ath_ahb_read_cachesize(struct ath_common *common, int *csz)
{
	*csz = L1_CACHE_BYTES >> 2;
}

static bool ath_ahb_eeprom_read(struct ath_common *common, u32 off, u16 *data)
{
	ath_err(common, "%s: eeprom data has to be provided externally\n",
		__func__);
	return false;
}

static const struct ath_bus_ops ath_ahb_bus_ops  = {
	.ath_bus_type = ATH_AHB,
	.read_cachesize = ath_ahb_read_cachesize,
	.eeprom_read = ath_ahb_eeprom_read,
};

#ifdef CONFIG_OF

#define QCA955X_DDR_CTL_CONFIG          0x108
#define QCA955X_DDR_CTL_CONFIG_ACT_WMAC BIT(23)

static int of_get_wifi_cal(struct device_node *np, struct ath9k_platform_data *pdata)
{
#ifdef CONFIG_MTD
	struct device_node *mtd_np = NULL;
	size_t retlen;
	int size, ret;
	struct mtd_info *mtd;
	const char *part;
	const __be32 *list;
	phandle phandle;

	list = of_get_property(np, "mtd-cal-data", &size);
	if (!list)
		return 0;

	if (size != (2 * sizeof(*list)))
		return 1;

	phandle = be32_to_cpup(list++);
	if (phandle)
		mtd_np = of_find_node_by_phandle(phandle);

	if (!mtd_np)
		return 1;

	part = of_get_property(mtd_np, "label", NULL);
	if (!part)
		part = mtd_np->name;

	mtd = get_mtd_device_nm(part);
	if (IS_ERR(mtd))
		return 1;

	ret = mtd_read(mtd, be32_to_cpup(list), sizeof(pdata->eeprom_data),
			&retlen, (u8*)pdata->eeprom_data);
	put_mtd_device(mtd);

#endif
	return 0;
}

static int ar913x_wmac_reset(void)
{
	ath79_device_reset_set(AR913X_RESET_AMBA2WMAC);
	mdelay(10);

	ath79_device_reset_clear(AR913X_RESET_AMBA2WMAC);
	mdelay(10);

	return 0;
}

static int ar933x_wmac_reset(void)
{
	int retries = 20;

	ath79_device_reset_set(AR933X_RESET_WMAC);
	ath79_device_reset_clear(AR933X_RESET_WMAC);

	while (1) {
		u32 bootstrap;

		bootstrap = ath79_reset_rr(AR933X_RESET_REG_BOOTSTRAP);
		if ((bootstrap & AR933X_BOOTSTRAP_EEPBUSY) == 0)
			return 0;

		if (retries-- == 0)
			break;

		udelay(10000);
	}

	pr_err("ar933x: WMAC reset timed out");
	return -ETIMEDOUT;
}

static int qca955x_wmac_reset(void)
{
	int i;

	/* Try to wait for WMAC DDR activity to stop */
	for (i = 0; i < 10; i++) {
		if (!(__raw_readl(ath79_ddr_base + QCA955X_DDR_CTL_CONFIG) &
		    QCA955X_DDR_CTL_CONFIG_ACT_WMAC))
			break;

		udelay(10);
	}

	ath79_device_reset_set(QCA955X_RESET_RTC);
	udelay(10);
	ath79_device_reset_clear(QCA955X_RESET_RTC);
	udelay(10);

	return 0;
}

enum {
	AR913X_WMAC = 0,
	AR933X_WMAC,
	AR934X_WMAC,
	QCA953X_WMAC,
	QCA955X_WMAC,
	QCA956X_WMAC,
};

static int ar9330_get_soc_revision(void)
{
	if (ath79_soc_rev == 1)
		return ath79_soc_rev;

	return 0;
}

static int ath79_get_soc_revision(void)
{
	return ath79_soc_rev;
}

static const struct of_ath_ahb_data {
	u16 dev_id;
	u32 bootstrap_reg;
	u32 bootstrap_ref;

	int (*soc_revision)(void);
	int (*wmac_reset)(void);
} of_ath_ahb_data[] = {
	[AR913X_WMAC] = {
		.dev_id = AR5416_AR9100_DEVID,
		.wmac_reset = ar913x_wmac_reset,

	},
	[AR933X_WMAC] = {
		.dev_id = AR9300_DEVID_AR9330,
		.bootstrap_reg = AR933X_RESET_REG_BOOTSTRAP,
		.bootstrap_ref = AR933X_BOOTSTRAP_REF_CLK_40,
		.soc_revision = ar9330_get_soc_revision,
		.wmac_reset = ar933x_wmac_reset,
	},
	[AR934X_WMAC] = {
		.dev_id = AR9300_DEVID_AR9340,
		.bootstrap_reg = AR934X_RESET_REG_BOOTSTRAP,
		.bootstrap_ref = AR934X_BOOTSTRAP_REF_CLK_40,
		.soc_revision = ath79_get_soc_revision,
	},
	[QCA953X_WMAC] = {
		.dev_id = AR9300_DEVID_AR953X,
		.bootstrap_reg = QCA953X_RESET_REG_BOOTSTRAP,
		.bootstrap_ref = QCA953X_BOOTSTRAP_REF_CLK_40,
		.soc_revision = ath79_get_soc_revision,
	},
	[QCA955X_WMAC] = {
		.dev_id = AR9300_DEVID_QCA955X,
		.bootstrap_reg = QCA955X_RESET_REG_BOOTSTRAP,
		.bootstrap_ref = QCA955X_BOOTSTRAP_REF_CLK_40,
		.wmac_reset = qca955x_wmac_reset,
	},
	[QCA956X_WMAC] = {
		.dev_id = AR9300_DEVID_QCA956X,
		.bootstrap_reg = QCA956X_RESET_REG_BOOTSTRAP,
		.bootstrap_ref = QCA956X_BOOTSTRAP_REF_CLK_40,
		.soc_revision = ath79_get_soc_revision,
	},
};

const struct of_device_id of_ath_ahb_match[] = {
	{ .compatible = "qca,ar9130-wmac", .data = &of_ath_ahb_data[AR913X_WMAC] },
	{ .compatible = "qca,ar9330-wmac", .data = &of_ath_ahb_data[AR933X_WMAC] },
	{ .compatible = "qca,ar9340-wmac", .data = &of_ath_ahb_data[AR934X_WMAC] },
	{ .compatible = "qca,qca9530-wmac", .data = &of_ath_ahb_data[QCA953X_WMAC] },
	{ .compatible = "qca,qca9550-wmac", .data = &of_ath_ahb_data[QCA955X_WMAC] },
	{ .compatible = "qca,qca9560-wmac", .data = &of_ath_ahb_data[QCA956X_WMAC] },
	{},
};
MODULE_DEVICE_TABLE(of, of_ath_ahb_match);

static int of_ath_ahb_probe(struct platform_device *pdev)
{
	struct ath9k_platform_data *pdata;
	const struct of_device_id *match;
	const struct of_ath_ahb_data *data;
	u8 led_pin;

	match = of_match_device(of_ath_ahb_match, &pdev->dev);
	data = (const struct of_ath_ahb_data *)match->data;

	pdata = dev_get_platdata(&pdev->dev);

	if (!of_property_read_u8(pdev->dev.of_node, "qca,led-pin", &led_pin))
		pdata->led_pin = led_pin;
	else
		pdata->led_pin = -1;

	if (of_property_read_bool(pdev->dev.of_node, "qca,disable-2ghz"))
		pdata->disable_2ghz = true;

	if (of_property_read_bool(pdev->dev.of_node, "qca,disable-5ghz"))
		pdata->disable_5ghz = true;

	if (of_property_read_bool(pdev->dev.of_node, "qca,tx-gain-buffalo"))
		pdata->tx_gain_buffalo = true;

	if (data->wmac_reset) {
		data->wmac_reset();
		pdata->external_reset = data->wmac_reset;
	}

	if (data->bootstrap_reg && data->bootstrap_ref) {
		u32 t = ath79_reset_rr(data->bootstrap_reg);
		if (t & data->bootstrap_ref)
			pdata->is_clk_25mhz = false;
		else
			pdata->is_clk_25mhz = true;
	}

	pdata->get_mac_revision = data->soc_revision;

	if (of_get_wifi_cal(pdev->dev.of_node, pdata))
		dev_err(&pdev->dev, "failed to load calibration data from mtd device\n");

	return data->dev_id;
}
#endif

static int ath_ahb_probe(struct platform_device *pdev)
{
	void __iomem *mem;
	struct ath_softc *sc;
	struct ieee80211_hw *hw;
	struct resource *res;
	const struct platform_device_id *id = platform_get_device_id(pdev);
	int irq;
	int ret = 0;
	struct ath_hw *ah;
	char hw_name[64];
	u16 dev_id;

	if (id)
		dev_id = id->driver_data;

#ifdef CONFIG_OF
	if (pdev->dev.of_node)
		pdev->dev.platform_data = devm_kzalloc(&pdev->dev,
					sizeof(struct ath9k_platform_data),
					GFP_KERNEL);
#endif

	if (!dev_get_platdata(&pdev->dev)) {
		dev_err(&pdev->dev, "no platform data specified\n");
		return -EINVAL;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "no memory resource found\n");
		return -ENXIO;
	}

	mem = devm_ioremap_nocache(&pdev->dev, res->start, resource_size(res));
	if (mem == NULL) {
		dev_err(&pdev->dev, "ioremap failed\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "no IRQ resource found\n");
		return -ENXIO;
	}

	irq = res->start;

	ath9k_fill_chanctx_ops();
	hw = ieee80211_alloc_hw(sizeof(struct ath_softc), &ath9k_ops);
	if (hw == NULL) {
		dev_err(&pdev->dev, "no memory for ieee80211_hw\n");
		return -ENOMEM;
	}

	SET_IEEE80211_DEV(hw, &pdev->dev);
	platform_set_drvdata(pdev, hw);

	sc = hw->priv;
	sc->hw = hw;
	sc->dev = &pdev->dev;
	sc->mem = mem;
	sc->irq = irq;

#ifdef CONFIG_OF
	dev_id = of_ath_ahb_probe(pdev);
#endif
	ret = request_irq(irq, ath_isr, IRQF_SHARED, "ath9k", sc);
	if (ret) {
		dev_err(&pdev->dev, "request_irq failed\n");
		goto err_free_hw;
	}

	ret = ath9k_init_device(dev_id, sc, &ath_ahb_bus_ops);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize device\n");
		goto err_irq;
	}

	ah = sc->sc_ah;
	ath9k_hw_name(ah, hw_name, sizeof(hw_name));
	wiphy_info(hw->wiphy, "%s mem=0x%lx, irq=%d\n",
		   hw_name, (unsigned long)mem, irq);

	return 0;

 err_irq:
	free_irq(irq, sc);
 err_free_hw:
	ieee80211_free_hw(hw);
	return ret;
}

static int ath_ahb_remove(struct platform_device *pdev)
{
	struct ieee80211_hw *hw = platform_get_drvdata(pdev);

	if (hw) {
		struct ath_softc *sc = hw->priv;

		ath9k_deinit_device(sc);
		free_irq(sc->irq, sc);
		ieee80211_free_hw(sc->hw);
	}
#ifdef CONFIG_OF
	pdev->dev.platform_data = NULL;
#endif

	return 0;
}

static struct platform_driver ath_ahb_driver = {
	.probe      = ath_ahb_probe,
	.remove     = ath_ahb_remove,
	.driver		= {
		.name	= "ath9k",
#ifdef CONFIG_OF
		.of_match_table = of_ath_ahb_match,
#endif
	},
	.id_table    = ath9k_platform_id_table,
};

MODULE_DEVICE_TABLE(platform, ath9k_platform_id_table);

int ath_ahb_init(void)
{
	return platform_driver_register(&ath_ahb_driver);
}

void ath_ahb_exit(void)
{
	platform_driver_unregister(&ath_ahb_driver);
}

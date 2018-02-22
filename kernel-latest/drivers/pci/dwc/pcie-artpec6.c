// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Axis ARTPEC-6 SoC
 *
 * Author: Niklas Cassel <niklas.cassel@axis.com>
 *
 * Based on work done by Phil Edworthy <phil@edworthys.org>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "pcie-designware.h"

#define to_artpec6_pcie(x)	dev_get_drvdata((x)->dev)

enum artpec_pcie_variants {
	ARTPEC6,
	ARTPEC7,
};

struct artpec6_pcie {
	struct dw_pcie		*pci;
	struct regmap		*regmap;	/* DT axis,syscon-pcie */
	void __iomem		*phy_base;	/* DT phy */
	enum artpec_pcie_variants variant;
	enum dw_pcie_device_mode mode;
};

struct artpec_pcie_of_data {
	enum artpec_pcie_variants variant;
	enum dw_pcie_device_mode mode;
};

static const struct of_device_id artpec6_pcie_of_match[];

/* PCIe Port Logic registers (memory-mapped) */
#define PL_OFFSET			0x700

#define ACK_F_ASPM_CTRL_OFF		(PL_OFFSET + 0xc)
#define ACK_N_FTS_MASK			GENMASK(15, 8)
#define ACK_N_FTS(x)			(((x) << 8) & ACK_N_FTS_MASK)

#define FAST_TRAINING_SEQ_MASK		GENMASK(7, 0)
#define FAST_TRAINING_SEQ(x)		(((x) << 0) & FAST_TRAINING_SEQ_MASK)

/* ARTPEC-6 specific registers */
#define PCIECFG				0x18
#define  PCIECFG_DBG_OEN		BIT(24)
#define  PCIECFG_CORE_RESET_REQ		BIT(21)
#define  PCIECFG_LTSSM_ENABLE		BIT(20)
#define  PCIECFG_DEVICE_TYPE_MASK	GENMASK(19, 16)
#define  PCIECFG_CLKREQ_B		BIT(11)
#define  PCIECFG_REFCLK_ENABLE		BIT(10)
#define  PCIECFG_PLL_ENABLE		BIT(9)
#define  PCIECFG_PCLK_ENABLE		BIT(8)
#define  PCIECFG_RISRCREN		BIT(4)
#define  PCIECFG_MODE_TX_DRV_EN		BIT(3)
#define  PCIECFG_CISRREN		BIT(2)
#define  PCIECFG_MACRO_ENABLE		BIT(0)
/* ARTPEC-7 specific fields */
#define  PCIECFG_REFCLKSEL		BIT(23)
#define  PCIECFG_NOC_RESET		BIT(3)

#define PCIESTAT			0x1c
/* ARTPEC-7 specific fields */
#define  PCIESTAT_EXTREFCLK		BIT(3)

#define NOCCFG				0x40
#define  NOCCFG_ENABLE_CLK_PCIE		BIT(4)
#define  NOCCFG_POWER_PCIE_IDLEACK	BIT(3)
#define  NOCCFG_POWER_PCIE_IDLE		BIT(2)
#define  NOCCFG_POWER_PCIE_IDLEREQ	BIT(1)

#define PHY_STATUS			0x118
#define  PHY_COSPLLLOCK			BIT(0)

#define PHY_TX_ASIC_OUT			0x4040
#define  PHY_TX_ASIC_OUT_TX_ACK		BIT(0)

#define PHY_RX_ASIC_OUT			0x405c
#define  PHY_RX_ASIC_OUT_ACK		BIT(0)

static u32 artpec6_pcie_readl(struct artpec6_pcie *artpec6_pcie, u32 offset)
{
	u32 val;

	regmap_read(artpec6_pcie->regmap, offset, &val);
	return val;
}

static void artpec6_pcie_writel(struct artpec6_pcie *artpec6_pcie, u32 offset, u32 val)
{
	regmap_write(artpec6_pcie->regmap, offset, val);
}

static u64 artpec6_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 pci_addr)
{
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	struct pcie_port *pp = &pci->pp;
	struct dw_pcie_ep *ep = &pci->ep;

	switch (artpec6_pcie->mode) {
	case DW_PCIE_RC_TYPE:
		return pci_addr - pp->cfg0_base;
	case DW_PCIE_EP_TYPE:
		return pci_addr - ep->phys_base;
	default:
		dev_err(pci->dev, "UNKNOWN device type\n");
	}
	return pci_addr;
}

static int artpec6_pcie_establish_link(struct dw_pcie *pci)
{
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	u32 val;

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	val |= PCIECFG_LTSSM_ENABLE;
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);

	return 0;
}

static void artpec6_pcie_stop_link(struct dw_pcie *pci)
{
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	u32 val;

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	val &= ~PCIECFG_LTSSM_ENABLE;
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.cpu_addr_fixup = artpec6_pcie_cpu_addr_fixup,
	.start_link = artpec6_pcie_establish_link,
	.stop_link = artpec6_pcie_stop_link,
};

static void artpec6_pcie_wait_for_phy_a6(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	struct device *dev = pci->dev;
	u32 val;
	unsigned int retries;

	retries = 50;
	do {
		usleep_range(1000, 2000);
		val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
		retries--;
	} while (retries &&
		(val & (NOCCFG_POWER_PCIE_IDLEACK | NOCCFG_POWER_PCIE_IDLE)));
	if (!retries)
		dev_err(dev, "PCIe clock manager did not leave idle state\n");

	retries = 50;
	do {
		usleep_range(1000, 2000);
		val = readl(artpec6_pcie->phy_base + PHY_STATUS);
		retries--;
	} while (retries && !(val & PHY_COSPLLLOCK));
	if (!retries)
		dev_err(dev, "PHY PLL did not lock\n");
}

static void artpec6_pcie_wait_for_phy_a7(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	struct device *dev = pci->dev;
	u32 val;
	u16 phy_status_tx, phy_status_rx;
	unsigned int retries;

	retries = 50;
	do {
		usleep_range(1000, 2000);
		val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
		retries--;
	} while (retries &&
		(val & (NOCCFG_POWER_PCIE_IDLEACK | NOCCFG_POWER_PCIE_IDLE)));
	if (!retries)
		dev_err(dev, "PCIe clock manager did not leave idle state\n");

	retries = 50;
	do {
		usleep_range(1000, 2000);
		phy_status_tx = readw(artpec6_pcie->phy_base + PHY_TX_ASIC_OUT);
		phy_status_rx = readw(artpec6_pcie->phy_base + PHY_RX_ASIC_OUT);
		retries--;
	} while (retries && ((phy_status_tx & PHY_TX_ASIC_OUT_TX_ACK) ||
				(phy_status_rx & PHY_RX_ASIC_OUT_ACK)));
	if (!retries)
		dev_err(dev, "PHY did not enter Pn state\n");
}

static void artpec6_pcie_wait_for_phy(struct artpec6_pcie *artpec6_pcie)
{
	switch (artpec6_pcie->variant) {
	case ARTPEC6:
		artpec6_pcie_wait_for_phy_a6(artpec6_pcie);
		break;
	case ARTPEC7:
		artpec6_pcie_wait_for_phy_a7(artpec6_pcie);
		break;
	}
}

static void artpec6_pcie_init_phy_a6(struct artpec6_pcie *artpec6_pcie)
{
	u32 val;

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	val |=  PCIECFG_RISRCREN |	/* Receiver term. 50 Ohm */
		PCIECFG_MODE_TX_DRV_EN |
		PCIECFG_CISRREN |	/* Reference clock term. 100 Ohm */
		PCIECFG_MACRO_ENABLE;
	val |= PCIECFG_REFCLK_ENABLE;
	val &= ~PCIECFG_DBG_OEN;
	val &= ~PCIECFG_CLKREQ_B;
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	usleep_range(5000, 6000);

	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	val |= NOCCFG_ENABLE_CLK_PCIE;
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
	usleep_range(20, 30);

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	val |= PCIECFG_PCLK_ENABLE | PCIECFG_PLL_ENABLE;
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	usleep_range(6000, 7000);

	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	val &= ~NOCCFG_POWER_PCIE_IDLEREQ;
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
}

static void artpec6_pcie_init_phy_a7(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	u32 val;
	bool extrefclk;

	/* Check if external reference clock is connected */
	val = artpec6_pcie_readl(artpec6_pcie, PCIESTAT);
	extrefclk = !!(val & PCIESTAT_EXTREFCLK);
	dev_dbg(pci->dev, "Using reference clock: %s\n",
		extrefclk ? "external" : "internal");

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	val |=  PCIECFG_RISRCREN |	/* Receiver term. 50 Ohm */
		PCIECFG_PCLK_ENABLE;
	if (extrefclk)
		val |= PCIECFG_REFCLKSEL;
	else
		val &= ~PCIECFG_REFCLKSEL;
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	usleep_range(10, 20);

	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	val |= NOCCFG_ENABLE_CLK_PCIE;
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
	usleep_range(20, 30);

	val = artpec6_pcie_readl(artpec6_pcie, NOCCFG);
	val &= ~NOCCFG_POWER_PCIE_IDLEREQ;
	artpec6_pcie_writel(artpec6_pcie, NOCCFG, val);
}

static void artpec6_pcie_init_phy(struct artpec6_pcie *artpec6_pcie)
{
	switch (artpec6_pcie->variant) {
	case ARTPEC6:
		artpec6_pcie_init_phy_a6(artpec6_pcie);
		break;
	case ARTPEC7:
		artpec6_pcie_init_phy_a7(artpec6_pcie);
		break;
	}
}

static void artpec6_pcie_set_nfts(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	u32 val;

	if (artpec6_pcie->variant != ARTPEC7)
		return;

	/*
	 * Increase the N_FTS (Number of Fast Training Sequences)
	 * to be transmitted when transitioning from L0s to L0.
	 */
	val = dw_pcie_readl_dbi(pci, ACK_F_ASPM_CTRL_OFF);
	val &= ~ACK_N_FTS_MASK;
	val |= ACK_N_FTS(180);
	dw_pcie_writel_dbi(pci, ACK_F_ASPM_CTRL_OFF, val);

	/*
	 * Set the Number of Fast Training Sequences that the core
	 * advertises as its N_FTS during Gen2 or Gen3 link training.
	 */
	val = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
	val &= ~FAST_TRAINING_SEQ_MASK;
	val |= FAST_TRAINING_SEQ(180);
	dw_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);
}

static void artpec6_pcie_assert_core_reset(struct artpec6_pcie *artpec6_pcie)
{
	u32 val;

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	switch (artpec6_pcie->variant) {
	case ARTPEC6:
		val |= PCIECFG_CORE_RESET_REQ;
		break;
	case ARTPEC7:
		val &= ~PCIECFG_NOC_RESET;
		break;
	}
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
}

static void artpec6_pcie_deassert_core_reset(struct artpec6_pcie *artpec6_pcie)
{
	u32 val;

	val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
	switch (artpec6_pcie->variant) {
	case ARTPEC6:
		val &= ~PCIECFG_CORE_RESET_REQ;
		break;
	case ARTPEC7:
		val |= PCIECFG_NOC_RESET;
		break;
	}
	artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
	usleep_range(100, 200);
}

static void artpec6_pcie_enable_interrupts(struct artpec6_pcie *artpec6_pcie)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	struct pcie_port *pp = &pci->pp;

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);
}

static int artpec6_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);

	artpec6_pcie_assert_core_reset(artpec6_pcie);
	artpec6_pcie_init_phy(artpec6_pcie);
	artpec6_pcie_deassert_core_reset(artpec6_pcie);
	artpec6_pcie_wait_for_phy(artpec6_pcie);
	artpec6_pcie_set_nfts(artpec6_pcie);
	dw_pcie_setup_rc(pp);
	artpec6_pcie_establish_link(pci);
	dw_pcie_wait_for_link(pci);
	artpec6_pcie_enable_interrupts(artpec6_pcie);

	return 0;
}

static const struct dw_pcie_host_ops artpec6_pcie_host_ops = {
	.host_init = artpec6_pcie_host_init,
};

static irqreturn_t artpec6_pcie_msi_handler(int irq, void *arg)
{
	struct artpec6_pcie *artpec6_pcie = arg;
	struct dw_pcie *pci = artpec6_pcie->pci;
	struct pcie_port *pp = &pci->pp;

	return dw_handle_msi_irq(pp);
}

static int artpec6_add_pcie_port(struct artpec6_pcie *artpec6_pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = artpec6_pcie->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = pci->dev;
	int ret;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pp->msi_irq < 0) {
			dev_err(dev, "failed to get MSI irq\n");
			return pp->msi_irq;
		}

		ret = devm_request_irq(dev, pp->msi_irq,
				       artpec6_pcie_msi_handler,
				       IRQF_SHARED | IRQF_NO_THREAD,
				       "artpec6-pcie-msi", artpec6_pcie);
		if (ret) {
			dev_err(dev, "failed to request MSI irq\n");
			return ret;
		}
	}

	pp->root_bus_nr = -1;
	pp->ops = &artpec6_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static void artpec6_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct artpec6_pcie *artpec6_pcie = to_artpec6_pcie(pci);
	enum pci_barno bar;

	artpec6_pcie_assert_core_reset(artpec6_pcie);
	artpec6_pcie_init_phy(artpec6_pcie);
	artpec6_pcie_deassert_core_reset(artpec6_pcie);
	artpec6_pcie_wait_for_phy(artpec6_pcie);
	artpec6_pcie_set_nfts(artpec6_pcie);

	for (bar = BAR_0; bar <= BAR_5; bar++)
		dw_pcie_ep_reset_bar(pci, bar);
}

static int artpec6_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
				  enum pci_epc_irq_type type, u8 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);

	switch (type) {
	case PCI_EPC_IRQ_LEGACY:
		dev_err(pci->dev, "EP cannot trigger legacy IRQs\n");
		return -EINVAL;
	case PCI_EPC_IRQ_MSI:
		return dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static struct dw_pcie_ep_ops pcie_ep_ops = {
	.ep_init = artpec6_pcie_ep_init,
	.raise_irq = artpec6_pcie_raise_irq,
};

static int artpec6_add_pcie_ep(struct artpec6_pcie *artpec6_pcie,
			       struct platform_device *pdev)
{
	int ret;
	struct dw_pcie_ep *ep;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = artpec6_pcie->pci;

	ep = &pci->ep;
	ep->ops = &pcie_ep_ops;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi2");
	pci->dbi_base2 = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->dbi_base2)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "addr_space");
	if (!res)
		return -EINVAL;

	ep->phys_base = res->start;
	ep->addr_size = resource_size(res);

	ret = dw_pcie_ep_init(ep);
	if (ret) {
		dev_err(dev, "failed to initialize endpoint\n");
		return ret;
	}

	return 0;
}

static int artpec6_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci;
	struct artpec6_pcie *artpec6_pcie;
	struct resource *dbi_base;
	struct resource *phy_base;
	int ret;
	const struct of_device_id *match;
	const struct artpec_pcie_of_data *data;
	enum artpec_pcie_variants variant;
	enum dw_pcie_device_mode mode;

	match = of_match_device(artpec6_pcie_of_match, dev);
	if (!match)
		return -EINVAL;

	data = (struct artpec_pcie_of_data *)match->data;
	variant = (enum artpec_pcie_variants)data->variant;
	mode = (enum dw_pcie_device_mode)data->mode;

	artpec6_pcie = devm_kzalloc(dev, sizeof(*artpec6_pcie), GFP_KERNEL);
	if (!artpec6_pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	artpec6_pcie->pci = pci;
	artpec6_pcie->variant = variant;
	artpec6_pcie->mode = mode;

	dbi_base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pci->dbi_base = devm_ioremap_resource(dev, dbi_base);
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);

	phy_base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	artpec6_pcie->phy_base = devm_ioremap_resource(dev, phy_base);
	if (IS_ERR(artpec6_pcie->phy_base))
		return PTR_ERR(artpec6_pcie->phy_base);

	artpec6_pcie->regmap =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"axis,syscon-pcie");
	if (IS_ERR(artpec6_pcie->regmap))
		return PTR_ERR(artpec6_pcie->regmap);

	platform_set_drvdata(pdev, artpec6_pcie);

	switch (artpec6_pcie->mode) {
	case DW_PCIE_RC_TYPE:
		if (!IS_ENABLED(CONFIG_PCIE_ARTPEC6_HOST))
			return -ENODEV;

		ret = artpec6_add_pcie_port(artpec6_pcie, pdev);
		if (ret < 0)
			return ret;
		break;
	case DW_PCIE_EP_TYPE: {
		u32 val;

		if (!IS_ENABLED(CONFIG_PCIE_ARTPEC6_EP))
			return -ENODEV;

		val = artpec6_pcie_readl(artpec6_pcie, PCIECFG);
		val &= ~PCIECFG_DEVICE_TYPE_MASK;
		artpec6_pcie_writel(artpec6_pcie, PCIECFG, val);
		ret = artpec6_add_pcie_ep(artpec6_pcie, pdev);
		if (ret < 0)
			return ret;
		break;
	}
	default:
		dev_err(dev, "INVALID device type %d\n", artpec6_pcie->mode);
	}

	return 0;
}

static const struct artpec_pcie_of_data artpec6_pcie_rc_of_data = {
	.variant = ARTPEC6,
	.mode = DW_PCIE_RC_TYPE,
};

static const struct artpec_pcie_of_data artpec6_pcie_ep_of_data = {
	.variant = ARTPEC6,
	.mode = DW_PCIE_EP_TYPE,
};

static const struct artpec_pcie_of_data artpec7_pcie_rc_of_data = {
	.variant = ARTPEC7,
	.mode = DW_PCIE_RC_TYPE,
};

static const struct artpec_pcie_of_data artpec7_pcie_ep_of_data = {
	.variant = ARTPEC7,
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id artpec6_pcie_of_match[] = {
	{
		.compatible = "axis,artpec6-pcie",
		.data = &artpec6_pcie_rc_of_data,
	},
	{
		.compatible = "axis,artpec6-pcie-ep",
		.data = &artpec6_pcie_ep_of_data,
	},
	{
		.compatible = "axis,artpec7-pcie",
		.data = &artpec7_pcie_rc_of_data,
	},
	{
		.compatible = "axis,artpec7-pcie-ep",
		.data = &artpec7_pcie_ep_of_data,
	},
	{},
};

static struct platform_driver artpec6_pcie_driver = {
	.probe = artpec6_pcie_probe,
	.driver = {
		.name	= "artpec6-pcie",
		.of_match_table = artpec6_pcie_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(artpec6_pcie_driver);

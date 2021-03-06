/*
 * drivers/usb/host/ehci-orion.c
 *
 * Tzachi Perelstein <tzachi@marvell.com>
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mbus.h>
#include <linux/clk.h>
#include <linux/platform_data/usb-ehci-orion.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

#include "ehci.h"

#define rdl(off)	readl_relaxed(hcd->regs + (off))
#define wrl(off, val)	writel_relaxed((val), hcd->regs + (off))

#define USB_CMD			0x140
#define   USB_CMD_RUN		BIT(0)
#define   USB_CMD_RESET		BIT(1)
#define USB_MODE		0x1a8
#define   USB_MODE_MASK		GENMASK(1, 0)
#define   USB_MODE_DEVICE	0x2
#define   USB_MODE_HOST		0x3
#define   USB_MODE_SDIS		BIT(4)
#define USB_CAUSE		0x310
#define USB_MASK		0x314
#define USB_WINDOW_CTRL(i)	(0x320 + ((i) << 4))
#define USB_WINDOW_BASE(i)	(0x324 + ((i) << 4))
#define USB_IPG			0x360
#define USB_PHY_PWR_CTRL	0x400
#define USB_PHY_TX_CTRL		0x420
#define USB_PHY_RX_CTRL		0x430
#define USB_PHY_IVREF_CTRL	0x440
#define USB_PHY_TST_GRP_CTRL	0x450

#define DRIVER_DESC "EHCI orion driver"

#define hcd_to_orion_priv(h) ((struct orion_ehci_hcd *)hcd_to_ehci(h)->priv)

struct orion_ehci_hcd {
	struct clk *clk;
	struct phy *phy;
};

static const char hcd_name[] = "ehci-orion";

static struct hc_driver __read_mostly ehci_orion_hc_driver;

#ifdef CONFIG_USB_MARVELL_ERRATA_FE_9049667
/*
 * In a370 and axp USB UTMI PHY there is an errata which causes
 * error in detection of high speed devices. For certain devices
 * with low pull up values the USB MAC doesnt detect the end of the
 * device chirp K signal and therefore remains stuck in reset
 * state.
 * The workaround solves this issue by modifying the UTMI PHY
 * squelch threshold once a high speed port reset error is detected.
 * Modifying the squelch level enables the MAC to detect the end of
 * device chirp K signal and to come out of reset. Once the MAC
 * comes out of reset a consecutive reset attempt is made by the USB stack.
 * This reset attempt succeeds due to the updated squelch level.
 *
 * Since the optimal squelch level is device dependant the WA
 * toggles between 2 verfied squelch levels 0xA and 0xE.
 */
#define MAX_EHCI_PORTS		3
#define PHY_RX_CTRL_REG_OFFSET(x) (0x708 + (0x40 * (x)))
#define SQUELCH_TH_OFFSET	4
#define SQUELCH_TH_MASK		0xF

static int hs_wa_applied[MAX_EHCI_PORTS] = {0};

static void ehci_marvell_toggle_squelch(struct ehci_hcd *ehci, int busnum)
{
	u32 __iomem *phy_rx_ctrl_reg;
	u32 val, squelch_th;

	phy_rx_ctrl_reg = (u32 __iomem *)(((u8 __iomem *)ehci->regs)
			+ PHY_RX_CTRL_REG_OFFSET(busnum - 1));

	val = ehci_readl(ehci, phy_rx_ctrl_reg);

	squelch_th = (val >> SQUELCH_TH_OFFSET) & SQUELCH_TH_MASK;
	if (squelch_th == 0xA)
		squelch_th = 0xE;
	else
		squelch_th = 0xA;

	val &= ~(SQUELCH_TH_MASK << SQUELCH_TH_OFFSET);
	val |= (squelch_th & SQUELCH_TH_MASK) << SQUELCH_TH_OFFSET;

	ehci_writel(ehci, val, phy_rx_ctrl_reg);
}

void ehci_marvell_hs_detect_wa_done(struct usb_device *udev)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);
	struct ehci_hcd	*ehci = hcd_to_ehci(hcd);
	int busnum = hcd->self.busnum;

	if (hs_wa_applied[busnum])
		ehci_marvell_toggle_squelch(ehci, busnum);

	hs_wa_applied[busnum] = 0;
}

int ehci_marvell_hs_detect_wa(struct ehci_hcd *ehci, int busnum)
{
	u32 __iomem *portsc_reg;
	u32 val = 0;
	u32 timeout;

	/* Apply the WA only once in a reset cycle */
	if (hs_wa_applied[busnum]++)
		return 1;

	ehci_marvell_toggle_squelch(ehci, busnum);

	/*
	 * After the squelch value is replaced we need to
	 * wait upto 3ms for the MAC to leave reset state.
	 */
	portsc_reg = &ehci->regs->port_status[0];
	timeout = 30;
	while (timeout--) {
		udelay(100);
		val = ehci_readl(ehci, portsc_reg);
		if ((val & PORT_RESET) == 0)
			break;
	}

	/* Return error If the MAC doesn't come out of reset */
	if (val & PORT_RESET)
		return 1;

	/*
	 * Clear Connect Status Change, Port Enable, and Port Enable Change.
	 * This returns the port status to pre-reset state and allows for
	 * succesfull consecutive reset.
	 */
	val = ehci_readl(ehci, portsc_reg);
	val = val  & (~PORT_PE);
	val = (val  & (~PORT_RWC_BITS)) | PORT_CSC | PORT_PEC;
	ehci_writel(ehci, val, portsc_reg);

	return 0;
}
#endif /* CONFIG_USB_MARVELL_ERRATA_FE_9049667 */

/*
 * Implement Orion USB controller specification guidelines
 */
static void orion_usb_phy_v1_setup(struct usb_hcd *hcd)
{
	/* The below GLs are according to the Orion Errata document */
	/*
	 * Clear interrupt cause and mask
	 */
	wrl(USB_CAUSE, 0);
	wrl(USB_MASK, 0);

	/*
	 * Reset controller
	 */
	wrl(USB_CMD, rdl(USB_CMD) | USB_CMD_RESET);
	while (rdl(USB_CMD) & USB_CMD_RESET);

	/*
	 * GL# USB-10: Set IPG for non start of frame packets
	 * Bits[14:8]=0xc
	 */
	wrl(USB_IPG, (rdl(USB_IPG) & ~0x7f00) | 0xc00);

	/*
	 * GL# USB-9: USB 2.0 Power Control
	 * BG_VSEL[7:6]=0x1
	 */
	wrl(USB_PHY_PWR_CTRL, (rdl(USB_PHY_PWR_CTRL) & ~0xc0)| 0x40);

	/*
	 * GL# USB-1: USB PHY Tx Control - force calibration to '8'
	 * TXDATA_BLOCK_EN[21]=0x1, EXT_RCAL_EN[13]=0x1, IMP_CAL[6:3]=0x8
	 */
	wrl(USB_PHY_TX_CTRL, (rdl(USB_PHY_TX_CTRL) & ~0x78) | 0x202040);

	/*
	 * GL# USB-3 GL# USB-9: USB PHY Rx Control
	 * RXDATA_BLOCK_LENGHT[31:30]=0x3, EDGE_DET_SEL[27:26]=0,
	 * CDR_FASTLOCK_EN[21]=0, DISCON_THRESHOLD[9:8]=0, SQ_THRESH[7:4]=0x1
	 */
	wrl(USB_PHY_RX_CTRL, (rdl(USB_PHY_RX_CTRL) & ~0xc2003f0) | 0xc0000010);

	/*
	 * GL# USB-3 GL# USB-9: USB PHY IVREF Control
	 * PLLVDD12[1:0]=0x2, RXVDD[5:4]=0x3, Reserved[19]=0
	 */
	wrl(USB_PHY_IVREF_CTRL, (rdl(USB_PHY_IVREF_CTRL) & ~0x80003 ) | 0x32);

	/*
	 * GL# USB-3 GL# USB-9: USB PHY Test Group Control
	 * REG_FIFO_SQ_RST[15]=0
	 */
	wrl(USB_PHY_TST_GRP_CTRL, rdl(USB_PHY_TST_GRP_CTRL) & ~0x8000);

	/*
	 * Stop and reset controller
	 */
	wrl(USB_CMD, rdl(USB_CMD) & ~USB_CMD_RUN);
	wrl(USB_CMD, rdl(USB_CMD) | USB_CMD_RESET);
	while (rdl(USB_CMD) & USB_CMD_RESET);

	/*
	 * GL# USB-5 Streaming disable REG_USB_MODE[4]=1
	 * TBD: This need to be done after each reset!
	 * GL# USB-4 Setup USB Host mode
	 */
	wrl(USB_MODE, USB_MODE_SDIS | USB_MODE_HOST);
}

static void
ehci_orion_conf_mbus_windows(struct usb_hcd *hcd,
			     const struct mbus_dram_target_info *dram)
{
	int i;

	for (i = 0; i < 4; i++) {
		wrl(USB_WINDOW_CTRL(i), 0);
		wrl(USB_WINDOW_BASE(i), 0);
	}

	for (i = 0; i < dram->num_cs; i++) {
		const struct mbus_dram_window *cs = dram->cs + i;

		wrl(USB_WINDOW_CTRL(i), ((cs->size - 1) & 0xffff0000) |
					(cs->mbus_attr << 8) |
					(dram->mbus_dram_target_id << 4) | 1);
		wrl(USB_WINDOW_BASE(i), cs->base);
	}
}

static const struct ehci_driver_overrides orion_overrides __initconst = {
	.extra_priv_size =	sizeof(struct orion_ehci_hcd),
};

static int ehci_orion_drv_probe(struct platform_device *pdev)
{
	struct orion_ehci_data *pd = dev_get_platdata(&pdev->dev);
	const struct mbus_dram_target_info *dram;
	struct resource *res;
	struct usb_hcd *hcd;
	struct ehci_hcd *ehci;
	void __iomem *regs;
	int irq, err;
	enum orion_ehci_phy_ver phy_version;
	struct orion_ehci_hcd *priv;

	if (usb_disabled())
		return -ENODEV;

	pr_debug("Initializing Orion-SoC USB Host Controller\n");

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0) {
		dev_err(&pdev->dev,
			"Found HC with no IRQ. Check %s setup!\n",
			dev_name(&pdev->dev));
		err = -ENODEV;
		goto err;
	}

	/*
	 * Right now device-tree probed devices don't get dma_mask
	 * set. Since shared usb code relies on it, set it here for
	 * now. Once we have dma capability bindings this can go away.
	 */
	err = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(regs)) {
		err = PTR_ERR(regs);
		goto err;
	}

	hcd = usb_create_hcd(&ehci_orion_hc_driver,
			&pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		err = -ENOMEM;
		goto err;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);
	hcd->regs = regs;

	ehci = hcd_to_ehci(hcd);
	ehci->caps = hcd->regs + 0x100;
	hcd->has_tt = 1;

	priv = hcd_to_orion_priv(hcd);
	/*
	 * Not all platforms can gate the clock, so it is not an error if
	 * the clock does not exists.
	 */
	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(priv->clk))
		clk_prepare_enable(priv->clk);

	priv->phy = devm_phy_optional_get(&pdev->dev, "usb");
	if (IS_ERR(priv->phy)) {
		err = PTR_ERR(priv->phy);
		if (err != -ENOSYS)
			goto err_phy_get;
	} else {
		err = phy_init(priv->phy);
		if (err)
			goto err_phy_init;

		err = phy_power_on(priv->phy);
		if (err)
			goto err_phy_power_on;
	}

	/*
	 * (Re-)program MBUS remapping windows if we are asked to.
	 */
	dram = mv_mbus_dram_info();
	if (dram)
		ehci_orion_conf_mbus_windows(hcd, dram);

	/*
	 * setup Orion USB controller.
	 */
	if (pdev->dev.of_node)
		phy_version = EHCI_PHY_NA;
	else
		phy_version = pd->phy_version;

	switch (phy_version) {
	case EHCI_PHY_NA:	/* dont change USB phy settings */
		break;
	case EHCI_PHY_ORION:
		orion_usb_phy_v1_setup(hcd);
		break;
	case EHCI_PHY_DD:
	case EHCI_PHY_KW:
	default:
		dev_warn(&pdev->dev, "USB phy version isn't supported.\n");
	}

	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err)
		goto err_add_hcd;

	device_wakeup_enable(hcd->self.controller);
	return 0;

err_add_hcd:
	if (!IS_ERR(priv->phy))
		phy_power_off(priv->phy);
err_phy_power_on:
	if (!IS_ERR(priv->phy))
		phy_exit(priv->phy);
err_phy_init:
err_phy_get:
	if (!IS_ERR(priv->clk))
		clk_disable_unprepare(priv->clk);
	usb_put_hcd(hcd);
err:
	dev_err(&pdev->dev, "init %s fail, %d\n",
		dev_name(&pdev->dev), err);

	return err;
}

static int ehci_orion_drv_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);
	struct orion_ehci_hcd *priv = hcd_to_orion_priv(hcd);

	usb_remove_hcd(hcd);

	if (!IS_ERR(priv->phy)) {
		phy_power_off(priv->phy);
		phy_exit(priv->phy);
	}

	if (!IS_ERR(priv->clk))
		clk_disable_unprepare(priv->clk);

	usb_put_hcd(hcd);

	return 0;
}

static const struct of_device_id ehci_orion_dt_ids[] = {
	{ .compatible = "marvell,orion-ehci", },
	{},
};
MODULE_DEVICE_TABLE(of, ehci_orion_dt_ids);

static struct platform_driver ehci_orion_driver = {
	.probe		= ehci_orion_drv_probe,
	.remove		= ehci_orion_drv_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver = {
		.name	= "orion-ehci",
		.of_match_table = ehci_orion_dt_ids,
	},
};

static int __init ehci_orion_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	pr_info("%s: " DRIVER_DESC "\n", hcd_name);

	ehci_init_driver(&ehci_orion_hc_driver, &orion_overrides);
	return platform_driver_register(&ehci_orion_driver);
}
module_init(ehci_orion_init);

static void __exit ehci_orion_cleanup(void)
{
	platform_driver_unregister(&ehci_orion_driver);
}
module_exit(ehci_orion_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_ALIAS("platform:orion-ehci");
MODULE_AUTHOR("Tzachi Perelstein");
MODULE_LICENSE("GPL v2");

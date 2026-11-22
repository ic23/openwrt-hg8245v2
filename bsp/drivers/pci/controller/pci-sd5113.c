// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) PCI host controller */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#define SD5113_PCI_READY	0x80
#define SD5113_PCI_CFGADDR	0x20
#define SD5113_PCI_AHBADDR	0x24
#define SD5113_PCI_CFGCTL	0x28
#define SD5113_PCI_ISTATUS	0x40
#define SD5113_PCI_IMASK	0x44
#define SD5113_PCI_WIN0_LO	0x70
#define SD5113_PCI_WIN1_LO	0x74
#define SD5113_PCI_WIN0_HI	0x94
#define SD5113_PCI_WIN1_HI	0x98

/* CONFIRMED: physical base of the separate sysctrl/CRG block used only
 * for the PCI clock-enable dance. Do NOT confuse with the PCI host
 * register window at 0x30000000. */
#define SD5113_SYSCTRL_PHYS	0x16140000
#define SD5113_SYSCTRL_SIZE	0x100
#define SD5113_SYSCTRL_PCICLK	0x1c
#define SD5113_SYSCTRL_PCICLK2	0x24
#define SD5113_SYSCTRL_PCICLK_BIT BIT(27)

#define SD5113_CFG_TIMEOUT	10000	/* vendor retry count, CONFIRMED */

struct sd5113_pci {
	struct device *dev;
	void __iomem *base;		/* 0x30000000, PCI host regs */
	void __iomem *sysctrl;		/* 0x16140000, clock-enable only */
	void __iomem *scratch_cpu;	/* CPU-side view of transaction buffer */
	dma_addr_t scratch_dma;	/* AHB/bus-side address of same buffer */
};

static int sd5113_pci_wait_ready(struct sd5113_pci *p)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if (readl(p->base + SD5113_PCI_READY) & BIT(0))
			return 0;
		udelay(10);
	}
	dev_err(p->dev, "pci logic is not ready!\n");
	return -ETIMEDOUT;
}

/* CONFIRMED arithmetic, transcribed directly from the disassembled
 * vendor encoder at raw offset 0x28c80. */
static u32 sd5113_pci_encode_cfg(unsigned int bus_number, unsigned int devfn,
				  int where)
{
	unsigned int function = devfn & 0x7;
	unsigned int slot;

	if (bus_number != 0) {
		return (where & 0xfc) | (function << 8) |
		       ((bus_number & 0xff) << 16) | BIT(0);
	}

	slot = (devfn >> 3) & 0x1f;
	if (slot > 15)
		slot = 15;

	return (where & 0xfc) | (function << 8) | BIT(16 + slot);
}

/* CONFIRMED sequence and offsets, from vendor raw offset 0x28be4.
 * HYPOTHESIS: exact coherency handling around scratch_cpu - vendor
 * cache-maintenance calls at this site were not fully identified. */
static int sd5113_pci_do_transaction(struct sd5113_pci *p, u32 cfgaddr)
{
	int i;

	writel(cfgaddr, p->base + SD5113_PCI_CFGADDR);
	writel(p->scratch_dma, p->base + SD5113_PCI_AHBADDR);
	writel(0x4a8, p->base + SD5113_PCI_CFGCTL);
	writel(0x4a9, p->base + SD5113_PCI_CFGCTL);

	for (i = 0; i < SD5113_CFG_TIMEOUT; i++) {
		if (readl(p->base + SD5113_PCI_CFGCTL) & BIT(0))
			return 0;
	}

	dev_err(p->dev, "pci_config_dma timeout!\n");
	return -ETIMEDOUT;
}

static int sd5113_pci_config_read(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct sd5113_pci *p = bus->sysdata;
	u32 cfgaddr = sd5113_pci_encode_cfg(bus->number, devfn, where);
	u32 data;
	int ret;

	ret = sd5113_pci_do_transaction(p, cfgaddr);
	if (ret) {
		*val = 0xffffffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	data = readl(p->scratch_cpu);
	data >>= (where & 3) * 8;

	switch (size) {
	case 1:
		*val = data & 0xff;
		break;
	case 2:
		*val = data & 0xffff;
		break;
	default:
		*val = data;
		break;
	}
	return PCIBIOS_SUCCESSFUL;
}

static int sd5113_pci_config_write(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	struct sd5113_pci *p = bus->sysdata;
	u32 cfgaddr = sd5113_pci_encode_cfg(bus->number, devfn, where);
	u32 data;
	unsigned int shift;
	int ret;

	switch (size) {
	case 1:
	case 2:
		ret = sd5113_pci_do_transaction(p, cfgaddr);
		if (ret)
			return PCIBIOS_DEVICE_NOT_FOUND;
		data = readl(p->scratch_cpu);

		shift = (where & 3) * 8;
		if (size == 1)
			data = (data & ~(0xffu << shift)) | ((val & 0xff) << shift);
		else
			data = (data & ~(0xffffu << shift)) | ((val & 0xffff) << shift);

		writel(data, p->scratch_cpu);
		break;
	default:
		writel(val, p->scratch_cpu);
		break;
	}

	return sd5113_pci_do_transaction(p, cfgaddr) ?
		PCIBIOS_DEVICE_NOT_FOUND : PCIBIOS_SUCCESSFUL;
}

static struct pci_ops sd5113_pci_ops = {
	.read	= sd5113_pci_config_read,
	.write	= sd5113_pci_config_write,
};

/* CONFIRMED sequence (offsets, values, and the associated "pci clock is
 * on!" log line), from vendor raw offset ~0xb318-0xb358. */
static void sd5113_pci_enable_clock(struct sd5113_pci *p)
{
	u32 val;

	val = readl(p->sysctrl + SD5113_SYSCTRL_PCICLK);
	val &= ~SD5113_SYSCTRL_PCICLK_BIT;
	writel(val, p->sysctrl + SD5113_SYSCTRL_PCICLK);

	writel(0x20000000, p->sysctrl + SD5113_SYSCTRL_PCICLK2);

	udelay(200); /* vendor used a 200-iteration busy-wait, exact
		      * cycle time not characterized - conservative udelay */

	val = readl(p->sysctrl + SD5113_SYSCTRL_PCICLK);
	val |= SD5113_SYSCTRL_PCICLK_BIT;
	writel(val, p->sysctrl + SD5113_SYSCTRL_PCICLK);

	dev_info(p->dev, "pci clock is on!\n");
}

/* CONFIRMED writes (values and offsets), UNCONFIRMED semantics - see
 * file header. Reproduced as-is because we have no better model. */
static void sd5113_pci_postinit_windows(struct sd5113_pci *p)
{
	writel(0x90000001, p->base + SD5113_PCI_WIN0_LO);
	writel(0x90000001, p->base + SD5113_PCI_WIN1_LO);
	writel(0xd0000000, p->base + SD5113_PCI_WIN0_HI);
	writel(0xd0000000, p->base + SD5113_PCI_WIN1_HI);

	writel(0xffffffff, p->base + SD5113_PCI_IMASK);
	writel(0x01000000, p->base + SD5113_PCI_ISTATUS);
	dev_dbg(p->dev, "istatus and imask %u\n",
		readl(p->base + SD5113_PCI_ISTATUS));
	dev_dbg(p->dev, "istatus and imask %u\n",
		readl(p->base + SD5113_PCI_IMASK));
}

static int sd5113_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_pci *p;
	struct pci_host_bridge *bridge;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*p));
	if (!bridge)
		return -ENOMEM;

	p = pci_host_bridge_priv(bridge);
	p->dev = dev;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	p->sysctrl = devm_ioremap(dev, SD5113_SYSCTRL_PHYS, SD5113_SYSCTRL_SIZE);
	if (!p->sysctrl)
		return -ENOMEM;

	p->scratch_cpu = dma_alloc_coherent(dev, PAGE_SIZE, &p->scratch_dma,
					     GFP_KERNEL);
	if (!p->scratch_cpu)
		return -ENOMEM;

	ret = sd5113_pci_wait_ready(p);
	if (ret)
		goto err_free_scratch;

	sd5113_pci_enable_clock(p);
	sd5113_pci_postinit_windows(p);

	bridge->sysdata = p;
	bridge->ops = &sd5113_pci_ops;

	ret = pci_host_probe(bridge);
	if (ret)
		goto err_free_scratch;

	return 0;

err_free_scratch:
	dma_free_coherent(dev, PAGE_SIZE, p->scratch_cpu, p->scratch_dma);
	return ret;
}

static const struct of_device_id sd5113_pci_of_match[] = {
	{ .compatible = "hisilicon,sd5113-pci" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_pci_of_match);

static struct platform_driver sd5113_pci_driver = {
	.probe = sd5113_pci_probe,
	.driver = {
		.name = "sd5113-pci",
		.of_match_table = sd5113_pci_of_match,
	},
};
builtin_platform_driver(sd5113_pci_driver);

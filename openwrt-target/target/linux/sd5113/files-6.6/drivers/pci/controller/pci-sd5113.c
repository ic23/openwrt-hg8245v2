// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) PCI host controller */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SD5113_PCI_WR_CFGADDR	0x00
#define SD5113_PCI_WR_AHBADDR	0x04
#define SD5113_PCI_WR_CFGCTL	0x08
#define SD5113_PCI_READY	0x80
#define SD5113_PCI_CFGADDR	0x20
#define SD5113_PCI_AHBADDR	0x24
#define SD5113_PCI_CFGCTL	0x28
#define SD5113_PCI_ISTATUS	0x40
#define SD5113_PCI_IMASK	0x44
#define SD5113_PCI_INTX_ACK	BIT(24)
#define SD5113_PCI_WIN0_LO	0x70
#define SD5113_PCI_WIN1_LO	0x74
#define SD5113_PCI_WIN0_HI	0x94
#define SD5113_PCI_WIN1_HI	0x98

/* CONFIRMED: physical base of the separate sysctrl/CRG block used only
 * for the PCI clock-enable dance. Do NOT confuse with the PCI host
 * register window at 0x30000000. */
#define SD5113_SYSCTRL_PCICLK	0x1c
#define SD5113_SYSCTRL_PCICLK2	0x24
#define SD5113_SYSCTRL_PCICLK_BIT BIT(27)

#define SD5113_CFG_TIMEOUT_US	10000
#define SD5113_PCI_STATUS_W1C	((u32)(PCI_STATUS_PARITY | \
	PCI_STATUS_SIG_TARGET_ABORT | PCI_STATUS_REC_TARGET_ABORT | \
	PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_SIG_SYSTEM_ERROR | \
	PCI_STATUS_DETECTED_PARITY) << 16)

struct sd5113_pci {
	struct device *dev;
	u8 __iomem *base;
	struct regmap *sysctrl;
	__le32 *scratch_cpu;
	dma_addr_t scratch_dma;
	bool cfg_failed;
	bool onboard_wifi_only;
	int intx_parent_irq;
	struct irq_domain *intx_domain;
};

static void sd5113_pci_intx_enable(struct irq_data *data)
{
	struct sd5113_pci *p = irq_data_get_irq_chip_data(data);

	enable_irq(p->intx_parent_irq);
}

static void sd5113_pci_intx_disable(struct irq_data *data)
{
	struct sd5113_pci *p = irq_data_get_irq_chip_data(data);

	disable_irq_nosync(p->intx_parent_irq);
}

static struct irq_chip sd5113_pci_intx_chip = {
	.name = "sd5113-pci-intx",
	.irq_enable = sd5113_pci_intx_enable,
	.irq_disable = sd5113_pci_intx_disable,
};

static int sd5113_pci_intx_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &sd5113_pci_intx_chip, handle_simple_irq);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops sd5113_pci_intx_ops = {
	.map = sd5113_pci_intx_map,
	.xlate = irq_domain_xlate_onecell,
};

static irqreturn_t sd5113_pci_intx_irq(int irq, void *id)
{
	struct sd5113_pci *p = id;
	u32 mask = readl(p->base + SD5113_PCI_IMASK);

	/* The vendor invokes this host-side operation at Wi-Fi ISR entry. */
	writel(mask | SD5113_PCI_INTX_ACK, p->base + SD5113_PCI_IMASK);
	if (generic_handle_domain_irq(p->intx_domain, 0))
		return IRQ_NONE;
	return IRQ_HANDLED;
}

static void sd5113_pci_intx_remove(void *data)
{
	struct irq_domain *domain = data;
	unsigned int irq = irq_find_mapping(domain, 0);

	if (irq)
		irq_dispose_mapping(irq);
	irq_domain_remove(domain);
}

static int sd5113_pci_intx_init(struct platform_device *pdev,
				struct sd5113_pci *p)
{
	struct device_node *node;
	int ret;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	p->intx_parent_irq = ret;

	node = of_get_child_by_name(pdev->dev.of_node, "interrupt-controller");
	if (!node)
		return -EINVAL;
	p->intx_domain = irq_domain_add_linear(node, 1, &sd5113_pci_intx_ops, p);
	of_node_put(node);
	if (!p->intx_domain)
		return -ENOMEM;

	ret = devm_add_action_or_reset(p->dev, sd5113_pci_intx_remove,
				       p->intx_domain);
	if (ret)
		return ret;

	return devm_request_irq(p->dev, p->intx_parent_irq, sd5113_pci_intx_irq,
				IRQF_NO_AUTOEN | IRQF_NO_THREAD, "sd5113-pci", p);
}

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
static int sd5113_pci_encode_cfg(unsigned int bus, unsigned int devfn,
				 int where, int size, u32 *addr)
{
	if (where < 0 || where >= PCI_CFG_SPACE_SIZE ||
	    (size != 1 && size != 2 && size != 4) || (where & (size - 1)))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (bus) {
		*addr = (where & 0xfc) | (devfn << 8) | (bus << 16) | BIT(0);
	} else {
		if (PCI_SLOT(devfn) > 15)
			return PCIBIOS_DEVICE_NOT_FOUND;
		*addr = (where & 0xfc) | (PCI_FUNC(devfn) << 8) |
			BIT(16 + PCI_SLOT(devfn));
	}
	return PCIBIOS_SUCCESSFUL;
}

/* CONFIRMED sequence and offsets, from vendor raw offset 0x28be4.
 * HYPOTHESIS: exact coherency handling around scratch_cpu - vendor
 * cache-maintenance calls at this site were not fully identified. */
static int sd5113_pci_do_transaction(struct sd5113_pci *p, u32 cfgaddr,
				      bool write, u32 *data)
{
	unsigned int addr = write ? SD5113_PCI_WR_CFGADDR : SD5113_PCI_CFGADDR;
	unsigned int ahb = write ? SD5113_PCI_WR_AHBADDR : SD5113_PCI_AHBADDR;
	unsigned int ctl = write ? SD5113_PCI_WR_CFGCTL : SD5113_PCI_CFGCTL;
	u32 cmd = write ? 0x4b8 : 0x4a8;
	u32 status;
	int ret;

	if (p->cfg_failed)
		return -EIO;
	ret = readl_poll_timeout_atomic(p->base + ctl, status, !(status & BIT(0)),
					2, SD5113_CFG_TIMEOUT_US);
	if (ret)
		goto timeout;

	WRITE_ONCE(*p->scratch_cpu, cpu_to_le32(write ? *data : U32_MAX));
	dma_wmb();
	writel(cfgaddr, p->base + addr);
	writel(lower_32_bits(p->scratch_dma), p->base + ahb);
	writel(cmd, p->base + ctl);
	writel(cmd | BIT(0), p->base + ctl);

	ret = readl_poll_timeout_atomic(p->base + ctl, status, !(status & BIT(0)),
					2, SD5113_CFG_TIMEOUT_US);
	if (ret)
		goto timeout;
	if (!write) {
		dma_rmb();
		*data = le32_to_cpu(READ_ONCE(*p->scratch_cpu));
	}

	return 0;

timeout:
	/* Do not reuse the shared DMA mailbox after an unresolved transaction. */
	p->cfg_failed = true;
	dev_err_ratelimited(p->dev, "config %s timed out\n", write ? "write" : "read");
	return ret;
}

static int sd5113_pci_config_read(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct sd5113_pci *p = bus->sysdata;
	u32 cfgaddr, data;
	int ret;

	*val = U32_MAX;
	if (p->onboard_wifi_only && (bus->number || devfn != PCI_DEVFN(2, 0)))
		return PCIBIOS_DEVICE_NOT_FOUND;

	ret = sd5113_pci_encode_cfg(bus->number, devfn, where, size, &cfgaddr);
	if (ret)
		return ret;
	if (sd5113_pci_do_transaction(p, cfgaddr, false, &data))
		return PCIBIOS_DEVICE_NOT_FOUND;

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
	u32 cfgaddr, data = val, mask;
	unsigned int shift;
	int ret;

	if (p->onboard_wifi_only && (bus->number || devfn != PCI_DEVFN(2, 0)))
		return PCIBIOS_DEVICE_NOT_FOUND;

	ret = sd5113_pci_encode_cfg(bus->number, devfn, where, size, &cfgaddr);
	if (ret)
		return ret;

	if (size != 4) {
		ret = sd5113_pci_do_transaction(p, cfgaddr, false, &data);
		if (ret)
			return PCIBIOS_DEVICE_NOT_FOUND;

		shift = (where & 3) * 8;
		mask = (size == 1 ? 0xffU : 0xffffU) << shift;
		if ((where & ~3) == PCI_COMMAND)
			data &= ~SD5113_PCI_STATUS_W1C;
		data = (data & ~mask) | ((val << shift) & mask);
	}

	return sd5113_pci_do_transaction(p, cfgaddr, true, &data) ?
		PCIBIOS_DEVICE_NOT_FOUND : PCIBIOS_SUCCESSFUL;
}

static struct pci_ops sd5113_pci_ops = {
	.read	= sd5113_pci_config_read,
	.write	= sd5113_pci_config_write,
};

/* CONFIRMED sequence (offsets, values, and the associated "pci clock is
 * on!" log line), from vendor raw offset ~0xb318-0xb358. */
static int sd5113_pci_enable_clock(struct sd5113_pci *p)
{
	int ret;

	ret = regmap_write_bits(p->sysctrl, SD5113_SYSCTRL_PCICLK,
				SD5113_SYSCTRL_PCICLK_BIT, 0);
	if (ret)
		return ret;

	ret = regmap_write(p->sysctrl, SD5113_SYSCTRL_PCICLK2, BIT(29));
	if (ret)
		return ret;

	msleep(200);

	return regmap_write_bits(p->sysctrl, SD5113_SYSCTRL_PCICLK,
				 SD5113_SYSCTRL_PCICLK_BIT, SD5113_SYSCTRL_PCICLK_BIT);
}

/* CONFIRMED writes (values and offsets), UNCONFIRMED semantics - see
 * file header. Reproduced as-is because we have no better model. */
static void sd5113_pci_init_windows(struct sd5113_pci *p)
{
	writel(0x90000001, p->base + SD5113_PCI_WIN0_LO);
	writel(0x90000001, p->base + SD5113_PCI_WIN1_LO);
	writel(0xd0000000, p->base + SD5113_PCI_WIN0_HI);
	writel(0xd0000000, p->base + SD5113_PCI_WIN1_HI);
}

static int sd5113_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_pci *p;
	struct pci_host_bridge *bridge;
	struct pci_bus *child;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*p));
	if (!bridge)
		return -ENOMEM;

	p = pci_host_bridge_priv(bridge);
	p->dev = dev;
	p->onboard_wifi_only = of_machine_is_compatible("huawei,hg8245");

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	p->sysctrl = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(p->sysctrl))
		return dev_err_probe(dev, PTR_ERR(p->sysctrl), "cannot get CRG\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	if (!(readl(p->base + SD5113_PCI_READY) & BIT(0))) {
		ret = sd5113_pci_enable_clock(p);
		if (ret)
			return ret;
	}
	ret = sd5113_pci_wait_ready(p);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	ret = sd5113_pci_intx_init(pdev, p);
	if (ret)
		return dev_err_probe(dev, ret, "cannot set up Wi-Fi INTx\n");

	p->scratch_cpu = dma_alloc_coherent(dev, sizeof(*p->scratch_cpu),
					    &p->scratch_dma, GFP_KERNEL);
	if (!p->scratch_cpu)
		return -ENOMEM;

	sd5113_pci_init_windows(p);

	bridge->sysdata = p;
	bridge->ops = &sd5113_pci_ops;

	ret = pci_scan_root_bus_bridge(bridge);
	if (ret) {
		dma_free_coherent(dev, sizeof(*p->scratch_cpu), p->scratch_cpu,
				  p->scratch_dma);
		return ret;
	}

	writel(0xffffffff, p->base + SD5113_PCI_IMASK);
	writel(0x01000000, p->base + SD5113_PCI_ISTATUS);
	if (pci_has_flag(PCI_PROBE_ONLY)) {
		pci_bus_claim_resources(bridge->bus);
	} else {
		pci_bus_size_bridges(bridge->bus);
		pci_bus_assign_resources(bridge->bus);
		list_for_each_entry(child, &bridge->bus->children, node)
			pcie_bus_configure_settings(child);
	}
	pci_bus_add_devices(bridge->bus);
	platform_set_drvdata(pdev, bridge);
	return 0;
}

static void sd5113_pci_remove(struct platform_device *pdev)
{
	struct pci_host_bridge *bridge = platform_get_drvdata(pdev);
	struct sd5113_pci *p = pci_host_bridge_priv(bridge);
	u32 status;

	pci_lock_rescan_remove();
	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	pci_unlock_rescan_remove();

	if (readl_poll_timeout(p->base + SD5113_PCI_CFGCTL, status,
			       !(status & BIT(0)), 10, SD5113_CFG_TIMEOUT_US) ||
	    readl_poll_timeout(p->base + SD5113_PCI_WR_CFGCTL, status,
			       !(status & BIT(0)), 10, SD5113_CFG_TIMEOUT_US)) {
		dev_err(p->dev, "config engine still active; keeping DMA mailbox allocated\n");
		return;
	}
	dma_free_coherent(p->dev, sizeof(*p->scratch_cpu), p->scratch_cpu,
			  p->scratch_dma);
}

static const struct of_device_id sd5113_pci_of_match[] = {
	{ .compatible = "hisilicon,sd5113-pci" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_pci_of_match);

static struct platform_driver sd5113_pci_driver = {
	.probe = sd5113_pci_probe,
	.remove_new = sd5113_pci_remove,
	.driver = {
		.name = "sd5113-pci",
		.of_match_table = sd5113_pci_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sd5113_pci_driver);

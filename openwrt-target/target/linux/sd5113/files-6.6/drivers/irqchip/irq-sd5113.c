// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) primary interrupt controller */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#define SD5113_INTC_NR_IRQS	32

/* CONFIRMED offsets - do not change without re-verifying against the
 * vendor disassembly referenced above. */
#define SD5113_INTC_MASK_REG	0x08
#define SD5113_INTC_PEND_REG	0x30

struct sd5113_intc {
	void __iomem *base;
	struct irq_domain *domain;
	raw_spinlock_t lock;
};

/* Single primary controller on this SoC - no need for a list, and
 * set_handle_irq()'s callback signature carries no context pointer. */
static struct sd5113_intc *sd5113_intc_data;

static void sd5113_set_mask(struct sd5113_intc *intc, unsigned int hwirq,
			    bool mask)
{
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&intc->lock, flags);
	val = readl(intc->base + SD5113_INTC_MASK_REG);
	if (mask)
		val |= BIT(hwirq);
	else
		val &= ~BIT(hwirq);
	writel(val, intc->base + SD5113_INTC_MASK_REG);
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

static void sd5113_irq_mask(struct irq_data *d)
{
	sd5113_set_mask(irq_data_get_irq_chip_data(d), d->hwirq, true);
}

static void sd5113_irq_unmask(struct irq_data *d)
{
	sd5113_set_mask(irq_data_get_irq_chip_data(d), d->hwirq, false);
}

static void sd5113_irq_ack(struct irq_data *d)
{
}

static struct irq_chip sd5113_irq_chip = {
	.name		= "sd5113-intc",
	.irq_ack	= sd5113_irq_ack,
	.irq_mask	= sd5113_irq_mask,
	.irq_unmask	= sd5113_irq_unmask,
};

static void __irq_entry sd5113_handle_irq(struct pt_regs *regs)
{
	struct sd5113_intc *intc = sd5113_intc_data;
	u32 pending = readl(intc->base + SD5113_INTC_PEND_REG);

	while (pending) {
		unsigned int hwirq = __ffs(pending);

		pending &= ~BIT(hwirq);
		if (generic_handle_domain_irq(intc->domain, hwirq)) {
			sd5113_set_mask(intc, hwirq, true);
			pr_warn_ratelimited("sd5113-intc: masked unmapped IRQ %u\n",
					    hwirq);
		}
	}
}


static int sd5113_irq_domain_map(struct irq_domain *d, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	struct sd5113_intc *intc = d->host_data;

	irq_set_chip_and_handler(irq, &sd5113_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, intc);
	irq_set_probe(irq);
	return 0;
}

static const struct irq_domain_ops sd5113_irq_domain_ops = {
	.map = sd5113_irq_domain_map,
	.xlate = irq_domain_xlate_onecell,
};

static int __init sd5113_intc_of_init(struct device_node *node,
				       struct device_node *parent)
{
	struct sd5113_intc *intc;

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;
	raw_spin_lock_init(&intc->lock);

	intc->base = of_iomap(node, 0);
	if (!intc->base) {
		pr_err("sd5113-intc: failed to map registers\n");
		kfree(intc);
		return -ENOMEM;
	}

	/* CONFIRMED init sequence: mask everything before enabling any line */
	writel(0xffffffff, intc->base + SD5113_INTC_MASK_REG);
	writel(0xffffffff, intc->base + 0x00);

	intc->domain = irq_domain_add_linear(node, SD5113_INTC_NR_IRQS,
					      &sd5113_irq_domain_ops, intc);
	if (!intc->domain) {
		pr_err("sd5113-intc: failed to add irq domain\n");
		iounmap(intc->base);
		kfree(intc);
		return -ENOMEM;
	}

	/* P0 blocker resolved this round: install the real top-level
	 * dispatcher now that the pending register (+0x30) is confirmed. */
	sd5113_intc_data = intc;
	set_handle_irq(sd5113_handle_irq);

	return 0;
}

IRQCHIP_DECLARE(sd5113_intc, "hisilicon,sd5113-intc", sd5113_intc_of_init);

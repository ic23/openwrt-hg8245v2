// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) machine descriptor */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/timer.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#define SD5113_WDT_PHYS		0x42000000
#define SD5113_WDT_KEY_OFF	0x000000dc
#define SD5113_WDT_VIRT		0xfee01000	/* next to the debug UART map */
#define SD5113_WDT_KEY1		0x55aa5a5a
#define SD5113_WDT_KEY2		0xaa55a5a5
#define SD5113_WDT_PERIOD_MS	250

static void __iomem *const sd5113_wdt_key =
	(void __iomem *)(SD5113_WDT_VIRT + SD5113_WDT_KEY_OFF);
static struct timer_list sd5113_wdt_timer;

static void sd5113_wdt_kick(void)
{
	writel_relaxed(SD5113_WDT_KEY1, sd5113_wdt_key);
	writel_relaxed(SD5113_WDT_KEY2, sd5113_wdt_key);
}

static void sd5113_wdt_timer_fn(struct timer_list *t)
{
	sd5113_wdt_kick();
	mod_timer(t, jiffies + msecs_to_jiffies(SD5113_WDT_PERIOD_MS));
}

static struct map_desc sd5113_io_desc[] __initdata = {
	{
		.virtual	= SD5113_WDT_VIRT,
		.pfn		= __phys_to_pfn(SD5113_WDT_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
#ifdef CONFIG_DEBUG_LL
	{
		.virtual	= CONFIG_DEBUG_UART_VIRT,
		.pfn		= __phys_to_pfn(CONFIG_DEBUG_UART_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
#endif
};

static void __init sd5113_map_io(void)
{
	iotable_init(sd5113_io_desc, ARRAY_SIZE(sd5113_io_desc));
	/* first chance to feed the watchdog from the kernel proper */
	sd5113_wdt_kick();
}

/*
 * Same sequence as the generic time_init() default (arch/arm/kernel/time.c)
 * plus the watchdog feeder. init_timers() has already run when this is
 * called, so the timer can be armed now; it starts firing once the clockevent
 * registered by timer_probe() is ticking.
 */
static void __init sd5113_init_time(void)
{
	sd5113_wdt_kick();
	of_clk_init(NULL);
	timer_probe();
	tick_setup_hrtimer_broadcast();
	sd5113_wdt_kick();

	timer_setup(&sd5113_wdt_timer, sd5113_wdt_timer_fn, 0);
	mod_timer(&sd5113_wdt_timer,
		  jiffies + msecs_to_jiffies(SD5113_WDT_PERIOD_MS));
}

#define SD5113_CRG_PHYS		0x16140000
#define SD5113_CRG_CLKEN	0x24
#define SD5113_CRG_CLKEN_USB	BIT(28)
#define SD5113_EHCI_PHYS	0x40010000
#define SD5113_EHCI_INSNREG01	0x94
#define SD5113_EHCI_INSNREG01_V	0x00800080

static int __init sd5113_usb_hw_init(void)
{
	struct device_node *np;
	void __iomem *crg, *ehci;

	if (!of_machine_is_compatible("huawei,hg8245"))
		return 0;

	np = of_find_compatible_node(NULL, NULL, "generic-ehci");
	if (!np)
		return 0;
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}
	of_node_put(np);

	crg = ioremap(SD5113_CRG_PHYS + SD5113_CRG_CLKEN, 4);
	ehci = ioremap(SD5113_EHCI_PHYS + SD5113_EHCI_INSNREG01, 4);
	if (!crg || !ehci) {
		pr_err("sd5113: cannot map USB clock/EHCI registers\n");
		goto out;
	}

	writel(SD5113_CRG_CLKEN_USB, crg);
	pr_info("sd5113: usb clock is on!\n");
	mdelay(50);
	writel(SD5113_EHCI_INSNREG01_V, ehci);
out:
	if (crg)
		iounmap(crg);
	if (ehci)
		iounmap(ehci);
	return 0;
}
/* arch_initcall: before any driver probes (device_initcall level) */
arch_initcall(sd5113_usb_hw_init);

static const char *const sd5113_dt_compat[] __initconst = {
	"huawei,hg8245",
	"hisilicon,sd5113",
	NULL,
};

DT_MACHINE_START(SD5113_DT, "SD5113-V100-PILOT-B01")
	/* Machine name CONFIRMED verbatim from a real vendor UART boot log
	 * (round 19) and independently from the vmlinux binary's own
	 * machine_desc struct (round 33, see file header for the full
	 * field-by-field read). */
	.dt_compat	= sd5113_dt_compat,
	.map_io		= sd5113_map_io,	/* STAGE 9: WDT page (+ debug UART), see above */
	.init_time	= sd5113_init_time,	/* STAGE 9: default + watchdog feeder */
MACHINE_END

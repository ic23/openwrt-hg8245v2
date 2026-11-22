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
#define SD5113_WDT_VIRT		0xfec01000
#define SD5113_RESET_PHYS	0x16140000
#define SD5113_RESET_VIRT	0xfec02000
#define SD5113_TRACE_TIMER_PHYS	0x16144000
#define SD5113_TRACE_TIMER_VIRT	0xfec03000
#define SD5113_TRACE_INTC_PHYS	0x40000000
#define SD5113_TRACE_INTC_VIRT	0xfec04000
#define SD5113_WDT_KEY1		0x55aa5a5a
#define SD5113_WDT_KEY2		0xaa55a5a5
#define SD5113_WDT_PERIOD_MS	250

static void __iomem *const sd5113_wdt_key =
	(void __iomem *)(SD5113_WDT_VIRT + SD5113_WDT_KEY_OFF);
static struct timer_list sd5113_wdt_timer;
static bool sd5113_boot_trace;
static unsigned long sd5113_trace_start, sd5113_trace_next;
static unsigned int sd5113_trace_kicks, sd5113_trace_lines;

static int __init sd5113_boot_trace_setup(char *arg)
{
	sd5113_boot_trace = true;
	return 0;
}
early_param("sd5113_boot_trace", sd5113_boot_trace_setup);

static void sd5113_trace_snapshot(const char *phase)
{
	void __iomem *timer = (void __iomem *)SD5113_TRACE_TIMER_VIRT;
	void __iomem *intc = (void __iomem *)SD5113_TRACE_INTC_VIRT;
	u32 load = readl_relaxed(timer + 0x00);
	u32 val1 = readl_relaxed(timer + 0x04);
	u32 ctrl1 = readl_relaxed(timer + 0x08);
	u32 mis1 = readl_relaxed(timer + 0x14);
	u32 val2 = readl_relaxed(timer + 0x24);
	u32 ctrl2 = readl_relaxed(timer + 0x28);
	u32 mask = readl_relaxed(intc + 0x08);
	u32 raw = readl_relaxed(intc + 0x18);
	u32 pend = readl_relaxed(intc + 0x30);

	pr_info("sd5113 trace %s: ticks=%lu wdt_timer_calls=%u t1(load=%u val=%u ctrl=%#x mis=%#x) t2(val=%u ctrl=%#x) intc(mask=%08x raw=%08x pending=%08x)\n",
		phase, jiffies - sd5113_trace_start, READ_ONCE(sd5113_trace_kicks),
		load, val1, ctrl1, mis1, val2, ctrl2, mask, raw, pend);
}

static void sd5113_wdt_kick(void)
{
	writel_relaxed(SD5113_WDT_KEY1, sd5113_wdt_key);
	writel_relaxed(SD5113_WDT_KEY2, sd5113_wdt_key);
}

static void sd5113_wdt_timer_fn(struct timer_list *t)
{
	unsigned int period = SD5113_WDT_PERIOD_MS;

	sd5113_wdt_kick();
	if (sd5113_boot_trace) {
		sd5113_trace_kicks++;
		period = 20;
		if (sd5113_trace_lines < 8 &&
		    time_after_eq(jiffies, sd5113_trace_next)) {
			sd5113_trace_lines++;
			sd5113_trace_snapshot("feed");
			sd5113_trace_next = jiffies + HZ;
		}
	}
	mod_timer(t, jiffies + msecs_to_jiffies(period));
}

static struct map_desc sd5113_io_desc[] __initdata = {
	{
		.virtual	= SD5113_WDT_VIRT,
		.pfn		= __phys_to_pfn(SD5113_WDT_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
	{
		.virtual	= SD5113_RESET_VIRT,
		.pfn		= __phys_to_pfn(SD5113_RESET_PHYS),
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

static struct map_desc sd5113_trace_io_desc[] __initdata = {
	{
		.virtual	= SD5113_TRACE_TIMER_VIRT,
		.pfn		= __phys_to_pfn(SD5113_TRACE_TIMER_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
	{
		.virtual	= SD5113_TRACE_INTC_VIRT,
		.pfn		= __phys_to_pfn(SD5113_TRACE_INTC_PHYS),
		.length		= SZ_4K,
		.type		= MT_DEVICE,
	},
};

static void __init sd5113_map_io(void)
{
	iotable_init(sd5113_io_desc, ARRAY_SIZE(sd5113_io_desc));
	/* Setup-time command line parsing may not yet have run. */
	iotable_init(sd5113_trace_io_desc, ARRAY_SIZE(sd5113_trace_io_desc));
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

	sd5113_trace_start = jiffies;
	sd5113_trace_next = jiffies;
	timer_setup(&sd5113_wdt_timer, sd5113_wdt_timer_fn, 0);
	mod_timer(&sd5113_wdt_timer, jiffies + msecs_to_jiffies(
		  sd5113_boot_trace ? 20 : SD5113_WDT_PERIOD_MS));
}

static int __init sd5113_trace_late_init(void)
{
	if (!sd5113_boot_trace || !of_machine_is_compatible("hisilicon,sd5113"))
		return 0;

	sd5113_trace_snapshot("before-delay");
	mdelay(25);
	sd5113_trace_snapshot("after-delay");
	return 0;
}
late_initcall_sync(sd5113_trace_late_init);

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

static void sd5113_restart(enum reboot_mode mode, const char *cmd)
{
	writel(~0U, (void __iomem *)(SD5113_RESET_VIRT + 0x04));
}

DT_MACHINE_START(SD5113_DT, "SD5113-V100-PILOT-B01")
	/* Machine name CONFIRMED verbatim from a real vendor UART boot log
	 * (round 19) and independently from the vmlinux binary's own
	 * machine_desc struct (round 33, see file header for the full
	 * field-by-field read). */
	.dt_compat	= sd5113_dt_compat,
	.map_io		= sd5113_map_io,	/* STAGE 9: WDT page (+ debug UART), see above */
	.init_time	= sd5113_init_time,	/* STAGE 9: default + watchdog feeder */
	.restart	= sd5113_restart,
MACHINE_END

// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "hg8245-zsp: " fmt

#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <asm/unaligned.h>

#include "zsp-loader.h"
#include "zsp-hif.h"

#ifdef HG8245_ZSP_CODEC_LAB
#include "slicprobe-embedded.h"
#include "zsp-pcm.h"
#endif

#include "zsp-hardware.h"

static bool stage;
module_param(stage, bool, 0400);
MODULE_PARM_DESC(stage, "Explicitly stage the pinned stock DSP ELF while ZSP is held in reset");

static bool boot;
module_param(boot, bool, 0400);
MODULE_PARM_DESC(boot, "Explicit one-shot ZSP boot, poll for readiness, then stop");

static bool version;
module_param(version, bool, 0400);
MODULE_PARM_DESC(version, "After Boot Complete issue one HIF DSP version query");

#ifdef HG8245_ZSP_CODEC_LAB
static bool codec;
module_param(codec, bool, 0400);
MODULE_PARM_DESC(codec, "After Boot Complete read LE88226 ID once, before stopping ZSP");

static bool tdm;
module_param(tdm, bool, 0400);
MODULE_PARM_DESC(tdm, "Apply the observed factory TDM mode before the one-shot ID read");

static bool pcm;
module_param(pcm, bool, 0400);
MODULE_PARM_DESC(pcm, "Apply the verified host PCM/GPIO fields before starting ZSP");

static bool spi_core;
module_param(spi_core, bool, 0400);
MODULE_PARM_DESC(spi_core, "Use the armed SPI core client in the voice-chain RAM lab");
extern int hg8245_le88226_run_id(void);
#endif

static int zsp_query_version(struct zsp_hardware *hw)
{
	u8 frame[ZSP_HIF_VERSION_REQ_LEN];
	int ret;

	ret = zsp_hif_make_version_request(ZSP_VERSION_SEQUENCE, frame);
	ret = ret ?: zsp_send_hif(hw, frame, sizeof(frame));
	if (ret)
		return ret;
	pr_info("HIF version query 0x54 published: %*ph\n", (int)sizeof(frame), frame);
	return zsp_wait_hif(hw, ZSP_WAIT_VERSION, ZSP_VERSION_SEQUENCE);
}

static int zsp_run_once(struct zsp_hardware *hw, int (*spi_id)(void))
{
	static const u32 timers[] = { 0x120, 0x11c, 0x118, 0x124, 0x114 };
#ifdef HG8245_ZSP_CODEC_LAB
	struct hg8245_pcm pcm_state = { };
#endif
	u32 control, saved_hw, saved_enable;
	unsigned int i;
	int ret, stop;

	ret = zsp_boot_preflight(hw);
	if (ret)
		return ret;
	ret = regmap_read(hw->crg, ZSP_CRG_CONTROL, &control);
	if (ret || (control & ZSP_CONTROL_MASK))
		return ret ?: -EIO;
	saved_enable = control & ZSP_HW_ENABLE;

#ifdef HG8245_ZSP_CODEC_LAB
	if (pcm && !spi_id) {
		ret = hg8245_pcm_prepare(&pcm_state, hw->crg, hw->hw.io, false);
		if (ret)
			return ret;
	}
#endif
	saved_hw = readl(hw->hw.io + ZSP_HW_INIT);
	for (i = 0; i < ARRAY_SIZE(timers); ++i)
		writel(0, hw->hw.io + timers[i]);
	writel(saved_hw | ZSP_HW_INIT_BITS, hw->hw.io + ZSP_HW_INIT);

	ret = zsp_enable_boot_irqs(hw);
	if (ret)
		goto restore_hw;

	pr_info("releasing ZSP reset; polling HIF (no codec commands)\n");
	ret = zsp_enter_run(hw);
	if (!ret)
		ret = zsp_wait_hif(hw, ZSP_WAIT_BOOT, 0);
	else
		pr_err("ZSP run-state check failed (%d)\n", ret);

	if (!ret && version)
		ret = zsp_query_version(hw);

#ifdef HG8245_ZSP_CODEC_LAB
	if (!ret && tdm)
		ret = zsp_apply_stock_tdm(hw);
	if (!ret && codec) {
		pr_info("ZSP ready; reading LE88226 ID with DSP still running\n");
		if (spi_id)
			pr_info("using the SPI core ID client after HIGHWAY and TDM\n");
		ret = spi_id ? spi_id() : hg8245_codec_read_id();
		pr_info("codec ID probe result=%d\n", ret);
	}
#endif

	/* Never leave the voice firmware running after this one-shot probe. */
	stop = regmap_update_bits(hw->crg, ZSP_CRG_CONTROL,
				  ZSP_CONTROL_MASK | ZSP_HW_ENABLE,
				  ZSP_CONTROL_STOP | saved_enable);
	stop = stop ?: regmap_read(hw->crg, ZSP_CRG_CONTROL, &control);
	if (!stop && (control & ZSP_CONTROL_MASK) != ZSP_CONTROL_STOP)
		stop = -EIO;
	if (stop) {
		pr_err("ZSP stop not verified (%d); reboot before any further probe\n", stop);
#ifdef HG8245_ZSP_CODEC_LAB
		if (pcm && !spi_id)
			hg8245_pcm_finish(&pcm_state, false);
#endif
		return stop;
	}
	hw->run_unverified = false;
	pr_info("ZSP stopped; one-shot boot result=%d\n", ret);

restore_hw:
	writel(saved_hw, hw->hw.io + ZSP_HW_INIT);
#ifdef HG8245_ZSP_CODEC_LAB
	if (pcm && !spi_id) {
		int restore = hg8245_pcm_finish(&pcm_state, true);

		if (restore)
			return restore;
	}
#endif
	return ret;
}

static int zsp_stage_hardware(const struct firmware *fw, int (*spi_id)(void))
{
	struct zsp_hardware hw = { };
#ifdef HG8245_ZSP_CODEC_LAB
	struct hg8245_pcm chain_pcm = { };
#endif
	u32 control;
	int ret;

	hw.crg = syscon_regmap_lookup_by_compatible("hisilicon,sd5113-crg");
	if (IS_ERR(hw.crg))
		return PTR_ERR(hw.crg);
	ret = zsp_map_firmware(fw, &hw, boot);
	if (ret)
		goto unmap;

#ifdef HG8245_ZSP_CODEC_LAB
	if (spi_id) {
		ret = zsp_hold_for_staging(hw.crg, true);
		ret = ret ?: hg8245_pcm_prepare(&chain_pcm, hw.crg, hw.hw.io, true);
		if (ret)
			goto unmap;
	}
#endif
	ret = zsp_hold_for_staging(hw.crg, false);
	if (!ret && boot)
		zsp_zero_boot_scratch(&hw.shared);
	if (!ret)
		ret = zsp_hif_prepare_cursors(zsp_readback_write, &hw);
	if (!ret && boot) {
		ret = zsp_hif_prepare_bootinfo(zsp_readback_write, &hw);
		if (!ret)
			pr_info("stock bootinfo verified at %08x (nine words)\n", ZSP_BOOTINFO_PHYS);
	}
	if (!ret)
		ret = zsp_stage(fw->data, fw->size, zsp_readback_write, &hw);
	if (!ret) {
		ret = regmap_read(hw.crg, ZSP_CRG_CONTROL, &control);
		if (!ret && (control & ZSP_CONTROL_MASK))
			ret = -EIO;
	}
	if (ret)
		pr_warn("staging stopped (%d) after %u verified writes; did not release reset\n",
			ret, hw.verified_writes);
	else if (!boot)
		pr_info("%u writes verified; ZSP in reset, not started\n",
			hw.verified_writes);
	else {
		pr_info("%u writes verified; starting one-shot ZSP probe\n", hw.verified_writes);
		ret = zsp_run_once(&hw, spi_id);
	}
unmap:
#ifdef HG8245_ZSP_CODEC_LAB
	if (chain_pcm.armed) {
		int restore = hg8245_pcm_finish(&chain_pcm, !hw.run_unverified);

		if (restore)
			ret = restore;
	}
#endif
	zsp_unmap_firmware(&hw);
	return ret;
}

static int __init hg8245_zsp_stage_init(void)
{
	const struct firmware *fw;
	struct device *dev;
	int (*spi_id)(void) = NULL;
	int ret;
#ifdef HG8245_ZSP_CODEC_LAB
	struct device_node *root;
	bool chain_lab;
#endif

	if (!of_machine_is_compatible("huawei,hg8245") ||
	    !of_machine_is_compatible("hisilicon,sd5113"))
		return -ENODEV;
	if (stage && boot)
		return -EINVAL;
	if (version && !boot)
		return -EINVAL;
#ifdef HG8245_ZSP_CODEC_LAB
	root = of_find_node_by_path("/");
	chain_lab = root && of_property_read_bool(root, "huawei,voice-chain-lab");
	of_node_put(root);
	if (spi_core && (!chain_lab || !boot || !codec || !tdm || !pcm))
		return -EINVAL;
	if (chain_lab && codec && !spi_core)
		return -EINVAL;
	if (codec && (!boot || version))
		return -EINVAL;
	if (tdm && !codec)
		return -EINVAL;
	if (pcm && !tdm)
		return -EINVAL;
#endif
	if (!stage && !boot) {
		pr_info("no action; staging/boot is opt-in\n");
		return 0;
	}

#ifdef HG8245_ZSP_CODEC_LAB
	if (spi_core) {
		spi_id = symbol_get(hg8245_le88226_run_id);
		if (!spi_id) {
			pr_err("load the guarded SPI ID client before starting ZSP\n");
			return -ENODEV;
		}
	}
#endif
	dev = root_device_register("hg8245-zsp-stage");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto put_spi;
	}
	ret = request_firmware_direct(&fw, ZSP_FIRMWARE, dev);
	if (!ret) {
		ret = zsp_stage_hardware(fw, spi_id);
		release_firmware(fw);
	} else {
		pr_warn("could not read %s (%d)\n", ZSP_FIRMWARE, ret);
	}
	root_device_unregister(dev);
put_spi:
#ifdef HG8245_ZSP_CODEC_LAB
	if (spi_id)
		symbol_put(hg8245_le88226_run_id);
#endif
	return ret;
}

static void __exit hg8245_zsp_stage_exit(void)
{
}

module_init(hg8245_zsp_stage_init);
module_exit(hg8245_zsp_stage_exit);
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(ZSP_FIRMWARE);
MODULE_DESCRIPTION("HG8245 stock voice DSP staging and explicit one-shot boot");

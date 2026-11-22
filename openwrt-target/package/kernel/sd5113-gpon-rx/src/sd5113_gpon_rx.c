// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/sd5113.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/random.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/sd5113-gem.h>
#include <asm/unaligned.h>

/* The SD5113 stock GPON PHY initializer uses these SYSCTL fields. */
static const struct {
	u16 reg;
	u32 mask;
} phy_fields[] = {
	{ 0x9c, BIT(0) }, { 0x18, BIT(26) }, { 0x44, GENMASK(20, 17) | BIT(23) | 7 },
	{ 0x1c, BIT(13) }, { 0x78, U32_MAX }, { 0x7c, U32_MAX },
};

#define OMCI_SLOT_SIZE 256
#define OMCI_QUEUE_SIZE 16

struct sd5113_plan {
	u32 paths[8]; /* unassigned = U32_MAX; otherwise GEM << 12 | Alloc-ID */
	u16 vid;
	u8 tag_pcp, unmarked, dscp[24];
	u8 filters[2][26];
};

struct sd5113_admitted_gem {
	u16 id;
	u32 saved;
};

struct sd5113_gpon_rx {
	void __iomem *omci_ready, *omci_data;
	void __iomem *omci_send, *omci_out;
	void __iomem *gem_table, *omci_route, *ploam_tx;
	int omcc_port;
	unsigned int session, admitted;
	struct sd5113_admitted_gem data[8];
	struct sd5113_plan plan;
	void __iomem *crypto; /* DGEM + 8 */
	DECLARE_BITMAP(encrypted, 4096);
	u8 next_key, key_banks;
	bool key_sent;
	unsigned long last_key;
	void __iomem *tcont, *random_delay;
	u8 owned_tcont;
	u16 discovery_delay;
	bool discovering;
	u32 ploam_tx_busy;
	bool omcc_seen, omcc_revoked;
	u8 omci_slots[OMCI_QUEUE_SIZE][OMCI_SLOT_SIZE];
	unsigned int head, count;
	u32 received, dropped;
	bool capture;
	void __iomem *los;
	void __iomem *lof;
	void __iomem *ploam_rx;
	void __iomem *ploam_crc_errors;
	void __iomem *ploam_pending;
	void __iomem *ploam_dropped;
	void __iomem *frame;
	void __iomem *pregap;
	void __iomem *commit;
	void __iomem *burst;
	struct delayed_work drain;
	struct mutex lock;
	struct regmap *crg;
	struct regmap *irq_regs;
	int irq;
	u32 old_mask;
	struct gpio_descs *pads;
	u32 saved_crg[ARRAY_SIZE(phy_fields)];
	u32 saved_funcs;
	unsigned int saved_fields;
	bool mux_changed;
	void __iomem *id_reg, *eqd_reg;
	u8 serial[8], password[10], onu_id;
	bool password_valid;
	bool serial_valid, ranged, disabled;
	u8 ranged_preamble;
	bool extended;
	u32 eqd_bits;
	unsigned long assign_deadline;
	u8 overhead[12];
	bool configured;
	bool stopping;
};

static void sd5113_phy_restore(void *data)
{
	struct sd5113_gpon_rx *rx = data;

	if (rx->mux_changed)
		sd5113_gpio_set_funcs(rx->pads->desc, rx->pads->ndescs, 0xc0, rx->saved_funcs, NULL);
	while (rx->saved_fields) {
		unsigned int i = --rx->saved_fields;

		regmap_update_bits(rx->crg, phy_fields[i].reg, phy_fields[i].mask, rx->saved_crg[i]);
	}
}

static int sd5113_phy_start(struct platform_device *pdev, struct sd5113_gpon_rx *rx)
{
	u32 mode, status;
	unsigned int i;
	int ret;

	rx->crg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "hisilicon,crg");
	if (IS_ERR(rx->crg))
		return PTR_ERR(rx->crg);
	for (i = 0; i < ARRAY_SIZE(phy_fields); i++) {
		ret = regmap_read(rx->crg, phy_fields[i].reg, &rx->saved_crg[i]);
		if (ret)
			return ret;
	}
	rx->pads = devm_gpiod_get_array(&pdev->dev, "pon", GPIOD_ASIS);
	if (IS_ERR(rx->pads))
		return PTR_ERR(rx->pads);
	if (rx->pads->ndescs != 2)
		return -EINVAL;
	ret = sd5113_gpio_set_funcs(rx->pads->desc, 2, 0, 0, &rx->saved_funcs);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, sd5113_phy_restore, rx);
	if (ret)
		return ret;
	rx->saved_fields = 1;
	ret = regmap_clear_bits(rx->crg, 0x9c, BIT(0));
	ret = ret ?: regmap_read(rx->crg, 0x9c, &mode);
	ret = ret ?: regmap_read(rx->crg, 0x80, &status);
	if (ret || (mode & BIT(0)))
		return ret ?: -ENODEV;
	if (status & BIT(0))
		return 0;

	rx->saved_fields = ARRAY_SIZE(phy_fields);
	ret = regmap_set_bits(rx->crg, 0x18, BIT(26));
	ret = ret ?: regmap_update_bits(rx->crg, 0x44, phy_fields[2].mask, BIT(23) | BIT(20) | 4);
	ret = ret ?: regmap_clear_bits(rx->crg, 0x1c, BIT(13));
	if (ret)
		return ret;
	udelay(5);
	ret = regmap_set_bits(rx->crg, 0x1c, BIT(13));
	if (ret)
		return ret;
	msleep(110);
	ret = regmap_clear_bits(rx->crg, 0x9c, BIT(0));
	ret = ret ?: regmap_read(rx->crg, 0x9c, &mode);
	if (ret || (mode & BIT(0)))
		return ret ?: -ENODEV;
	ret = regmap_write(rx->crg, 0x78, 0x10081681);
	ret = ret ?: regmap_write(rx->crg, 0x7c, 0x004ce7c5);
	ret = ret ?: regmap_set_bits(rx->crg, 0x78, BIT(4));
	ret = ret ?: regmap_set_bits(rx->crg, 0x78, BIT(5));
	if (ret)
		return ret;
	udelay(5);
	ret = regmap_clear_bits(rx->crg, 0x78, BIT(4));
	if (ret)
		return ret;
	udelay(5);
	ret = regmap_clear_bits(rx->crg, 0x78, BIT(5));
	if (ret)
		return ret;
	rx->mux_changed = true;
	ret = sd5113_gpio_set_funcs(rx->pads->desc, 2, BIT(6), BIT(6), NULL);
	return ret ?: sd5113_gpio_set_funcs(rx->pads->desc, 2, BIT(7), BIT(7), NULL);
}

#define ONU_UNASSIGNED 0xff

/* BoardInfo is a short XML document at the start of the board's own syspara. */
static int sd5113_parse_serial(char *buf, u8 serial[8])
{
	char *end, *tag, *attr;

	if (strncmp(buf, "<BoardInfo>", 11))
		return -EINVAL;
	end = strstr(buf, "</BoardInfo>");
	if (!end)
		return -EINVAL;
	*end = 0;
	tag = strstr(buf, "<sn ");
	if (!tag)
		return -EINVAL;
	end = strchr(tag, '>');
	if (!end)
		return -EINVAL;
	*end = 0;
	attr = strstr(tag, " cur_sn=\x22");
	if (!attr)
		return -EINVAL;
	attr += strlen(" cur_sn=\x22");
	if (strcspn(attr, "\x22") != 16 || attr[16] != '"' || hex2bin(serial, attr, 8))
		return -EINVAL;
	return memchr_inv(serial, 0, 8) && memchr_inv(serial, 0xff, 8) ? 0 : -EINVAL;
}

/* BoardInfo on the reference board has an explicitly empty snpassword.
 * Accept the full ten-octet hex encoding too. Never log the credential.
 */
static bool sd5113_board_password(const char *buf, u8 password[10])
{
	const char *key = "<snpassword infoStr=\"";
	const char *tag = strstr(buf, key);
	const char *end = strstr(buf, "</BoardInfo>");
	size_t n;

	if (!tag || !end || tag >= end || strncmp(buf, "<BoardInfo>", 11))
		return false;
	tag += strlen(key);
	n = strcspn(tag, "\"");
	if (tag + n >= end || strncmp(tag + n, "\"/>", 3))
		return false;
	if (n == 0)
		return true;
	return n == 20 && !hex2bin(password, tag, 10);
}

static int sd5113_activation_probe(struct platform_device *pdev, struct sd5113_gpon_rx *rx)
{
	struct device_node *np;
	struct mtd_info *mtd;
	size_t len = 0;
	char *buf;
	int ret;

	rx->onu_id = ONU_UNASSIGNED;
	rx->omcc_port = -1;
	np = of_parse_phandle(pdev->dev.of_node, "huawei,board-info", 0);
	if (!np)
		return 0;
	mtd = of_get_mtd_device_by_node(np);
	of_node_put(np);
	if (IS_ERR(mtd))
		return PTR_ERR(mtd);
	buf = kzalloc(4097, GFP_KERNEL);
	if (!buf) {
		put_mtd_device(mtd);
		return -ENOMEM;
	}
	ret = mtd_read(mtd, 0, 4096, &len, buf);
	put_mtd_device(mtd);
	if ((!ret || mtd_is_bitflip(ret)) && len == 4096) {
		rx->password_valid = sd5113_board_password(buf, rx->password);
		rx->serial_valid = !sd5113_parse_serial(buf, rx->serial);
	}
	kfree_sensitive(buf);
	if (!rx->serial_valid)
		dev_warn(&pdev->dev, "BoardInfo serial unavailable; ignoring addressed activation messages\n");

	rx->id_reg = devm_platform_ioremap_resource_byname(pdev, "onu-id");
	if (IS_ERR(rx->id_reg))
		return PTR_ERR(rx->id_reg);
	rx->eqd_reg = devm_platform_ioremap_resource_byname(pdev, "equalization-delay");
	return PTR_ERR_OR_ZERO(rx->eqd_reg);
}

static void sd5113_write_id(struct sd5113_gpon_rx *rx, u8 id)
{
	writel((readl(rx->id_reg) & ~0xffU) | id, rx->id_reg);
}

/* UGTC counts whole octets and remaining upstream bit periods separately. */
static void sd5113_write_eqd(struct sd5113_gpon_rx *rx, u32 bits)
{
	writel(bits >> 3, rx->eqd_reg);
	writel(bits & 7, rx->eqd_reg + 4);
	writel(0, rx->pregap);
	writel(readl(rx->commit) | BIT(0), rx->commit);
}

/* The low byte of each downstream DGEM entry selects the destination
 * and enables reception. Destination 1 delivers management frames to OMCI.
 */
static int sd5113_data_index(const struct sd5113_admitted_gem *data, int n, int gem)
{
	int i;

	for (i = 0; i < n; i++)
		if (data[i].id == gem)
			return i;
	return -1;
}

static void sd5113_clear_data(struct sd5113_gpon_rx *rx)
{
	unsigned int i;

	if (!rx->admitted)
		return;
	sd5113_eth_set_gem(NULL);
	for (i = 0; i < rx->admitted; i++) {
		void __iomem *entry = rx->gem_table + 4 * rx->data[i].id;

		writel(rx->data[i].saved, entry);
		readl(entry);
	}
	rx->admitted = 0;
}

static int sd5113_apply_plan(struct sd5113_gpon_rx *rx, const struct sd5113_plan *plan);

static void sd5113_clear_omcc(struct sd5113_gpon_rx *rx)
{
	sd5113_clear_data(rx);
	if (rx->gem_table && rx->omcc_port >= 0) {
		void __iomem *entry = rx->gem_table + 4 * rx->omcc_port;

		rx->session++;
		writel(readl(entry) & ~BIT(4), entry);
		readl(entry);
	}
	rx->omcc_port = -1;
	rx->omcc_seen = false;
	rx->head = rx->count = 0;
}

/* UGTC + 0x04..0x20: SD5113 selects one copy for ACK and
 * grant-driven repetition for Serial_Number_ONU. Clear repeat mode on exit.
 */
static void sd5113_clear_ploam_tx(struct sd5113_gpon_rx *rx)
{
	void __iomem *tx = rx->ploam_tx;
	u32 reset = readl(tx + 0x1c);

	writel(reset | 1, tx + 0x1c);
	writel(reset & ~1U, tx + 0x1c);
	writel(readl(tx + 0x14) & ~1U, tx + 0x14);
}

static int sd5113_send_ploam(struct sd5113_gpon_rx *rx, const u8 frame[12], unsigned int copies)
{
	void __iomem *tx = rx->ploam_tx;
	u32 status;
	int i;

	if (readl_poll_timeout(tx + 0x18, status, !(status & 1), 10, 1000)) {
		rx->ploam_tx_busy++;
		return -EBUSY;
	}
	writel((readl(tx + 0x14) & ~1U) | (copies == 0), tx + 0x14);
	writel(copies ?: 3, tx + 0x10);
	writel(readl(tx) | BIT(0), tx);
	for (i = 0; i < 12; i += 4)
		writel(get_unaligned_be32(frame + i), tx + 4 + i);
	writel(readl(tx + 0x18) | BIT(0), tx + 0x18);
	return 0;
}

static void sd5113_ack_ploam(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	u8 ack[12] = { rx->onu_id, 9, msg[1] };

	memcpy(ack + 3, msg, 9);
	sd5113_send_ploam(rx, ack, 1);
}

static void sd5113_set_tcont(struct sd5113_gpon_rx *rx, int slot, u16 id, bool enable)
{
	void __iomem *reg = rx->tcont + slot * 4;
	u32 word = readl(reg) & ~(BIT(31) | GENMASK(15, 0));

	writel(word | (enable ? BIT(31) : 0) | (id << 4) | slot, reg);
	if (!enable && rx->admitted) {
		struct sd5113_plan plan = rx->plan;
		unsigned int p;

		for (p = 0; p < ARRAY_SIZE(plan.paths); p++)
			if ((plan.paths[p] & 4095) == id)
				plan.paths[p] = U32_MAX;
		if (sd5113_apply_plan(rx, &plan))
			sd5113_clear_data(rx);
	}
	if (enable)
		rx->owned_tcont |= BIT(slot);
	else
		rx->owned_tcont &= ~BIT(slot);
}

static void sd5113_assign_alloc(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	u16 id = get_unaligned_be16(msg + 2) >> 4;
	int slot, free = -1;

	if (id < 256 || (msg[4] != 1 && msg[4] != 0xff))
		return;
	for (slot = 1; slot < 8; slot++) {
		u32 word = readl(rx->tcont + slot * 4);

		if ((word & BIT(31)) && ((word >> 4) & 0xfff) == id) {
			if (msg[4] == 0xff && (rx->owned_tcont & BIT(slot)))
				sd5113_set_tcont(rx, slot, id, false);
			break;
		}
		if (!(word & BIT(31)) && free < 0)
			free = slot;
	}
	if (slot == 8 && msg[4] == 1) {
		if (free < 0)
			return;
		sd5113_set_tcont(rx, free, id, true);
	}
	sd5113_ack_ploam(rx, msg);
}

static void sd5113_discover(struct sd5113_gpon_rx *rx, u8 id)
{
	u8 frame[12] = { id, 1 };
	u32 delay;

	sd5113_clear_ploam_tx(rx);
	if (id == ONU_UNASSIGNED) {
		sd5113_write_id(rx, ONU_UNASSIGNED);
		rx->discovery_delay = get_random_u32_below(234);
	}
	delay = rx->discovery_delay;
	writel((readl(rx->random_delay) & ~0xffffU) | (delay << 5), rx->random_delay);
	writel(readl(rx->commit) | BIT(0), rx->commit);
	memcpy(frame + 2, rx->serial, 8);
	frame[10] = delay >> 4;
	frame[11] = (delay << 4) | BIT(2);
	if (sd5113_send_ploam(rx, frame, 0))
		return;
	sd5113_set_tcont(rx, 0, id == ONU_UNASSIGNED ? 254 : id, true);
	rx->discovering = true;
	rx->assign_deadline = jiffies + 10 * HZ;
}

static void sd5113_configure_omcc(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	unsigned int id = get_unaligned_be16(msg + 3) >> 4;
	void __iomem *entry = rx->gem_table + 4 * id;

	sd5113_ack_ploam(rx, msg);
	if (!(msg[2] & 1)) {
		sd5113_clear_omcc(rx);
		return;
	}
	if (rx->omcc_port == id)
		return;
	sd5113_clear_omcc(rx);
	writel((readl(rx->omci_route) & ~GENMASK(30, 16)) | (id << 16), rx->omci_route);
	writel((readl(entry) & ~0xffU) | BIT(4) | 1, entry);
	if ((readl(entry) & 0xff) == 0x11 &&
	    ((readl(rx->omci_route) >> 16) & 0x7fff) == id)
		rx->omcc_port = id;
	else
		writel(readl(entry) & ~BIT(4), entry);
}

/* hi_dp: bank selector DGEM+0x14, eight BE halfwords in 32-bit
 * registers per bank at +0x18/+0x38. Only stage into the inactive bank.
 */
static void sd5113_request_key(struct sd5113_gpon_rx *rx)
{
	u8 key[16], frame[12] = { rx->onu_id, 5, rx->next_key };
	unsigned int bank, i;
	void __iomem *dst;

	if (!rng_is_initialized() || (rx->key_sent &&
	    time_before(jiffies, rx->last_key + msecs_to_jiffies(20))))
		return;
	get_random_bytes(key, sizeof(key));
	bank = !(readl(rx->crypto + 0x0c) & 1);
	dst = rx->crypto + 0x10 + 0x20 * bank;
	rx->key_sent = false;
	for (i = 0; i < 8; i++)
		writel(get_unaligned_be16(key + 2*i), dst + 4*i);
	readl(dst + 28);
	rx->key_banks |= BIT(bank);
	for (i = 0; i < 2; i++) {
		frame[3] = i;
		memcpy(frame + 4, key + i * 8, 8);
		if (sd5113_send_ploam(rx, frame, 3))
			goto out;
	}
	rx->key_sent = true;
	rx->next_key++;
	rx->last_key = jiffies;
out:
	memzero_explicit(frame, sizeof(frame));
	memzero_explicit(key, sizeof(key));
}

static void sd5113_encrypted_port(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	unsigned int gem = get_unaligned_be16(msg + 3) >> 4;
	void __iomem *entry = rx->gem_table + gem * 4;
	bool encrypt = msg[2] & 1;

	if (!(msg[2] & 2))
		return;
	__assign_bit(gem, rx->encrypted, encrypt);
	if (sd5113_data_index(rx->data, rx->admitted, gem) >= 0 || gem == rx->omcc_port) {
		u32 v = readl(entry);
		writel(encrypt ? v | BIT(3) : v & ~BIT(3), entry);
		readl(entry);
	}
	sd5113_ack_ploam(rx, msg);
}

static void sd5113_end_session(struct sd5113_gpon_rx *rx)
{
	if (rx->discovering || rx->owned_tcont) {
		int i;

		sd5113_clear_ploam_tx(rx);
		for (i = 0; i < 8; i++)
			if (rx->owned_tcont & BIT(i))
				sd5113_set_tcont(rx, i, 0xff, false);
		writel(readl(rx->random_delay) & ~0xffffU, rx->random_delay);
		writel(readl(rx->commit) | 1, rx->commit);
		rx->discovering = false;
	}
	if (rx->onu_id != ONU_UNASSIGNED) {
		rx->omcc_revoked = true;
		sd5113_write_id(rx, ONU_UNASSIGNED);
		if (rx->ranged)
			sd5113_write_eqd(rx, 0);
	}
	rx->onu_id = ONU_UNASSIGNED;
	if (rx->key_banks) {
		int bank, i;

		for (bank = 0; bank < 2; bank++)
			if (rx->key_banks & BIT(bank))
				for (i = 0; i < 8; i++)
					writel(0, rx->crypto + 0x10 + bank * 0x20 + i * 4);
		rx->key_banks = 0;
	}
	rx->next_key = 0;
	rx->key_sent = false;
	bitmap_zero(rx->encrypted, 4096);
	rx->ranged = false;
	rx->extended = false;
	rx->configured = false;
	sd5113_clear_omcc(rx);
}

/* Same 96-bit budget and 20-bit delimiter accounting as the SD5113
 * PLOAM_ReceiveUpstreamOverhead handler (HG8245V2 board profile).
 */
static void sd5113_set_overhead(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	unsigned int guard = msg[2], type1 = msg[3], type2 = msg[4];
	unsigned int used = guard + type1 + type2 + 20;
	u32 preeqd = (msg[9] & BIT(5)) ? get_unaligned_be16(msg + 10) : 0;

	if (guard < 32 || (guard & 7) || used > 96)
		return;
	if (rx->configured && (!rx->tcont || rx->discovering) && !memcmp(msg, rx->overhead, 12))
		return;
	writel(type1, rx->burst);
	writel(type2, rx->burst + 4);
	writel((96 - used) / 8, rx->burst + 8);
	writel(msg[5], rx->burst + 12);
	writel(get_unaligned_be24(msg + 6), rx->burst + 16);
	if (!rx->ranged && (readl(rx->pregap) & 0xffff) != preeqd) {
		writel(preeqd, rx->pregap);
		writel(readl(rx->commit) | BIT(0), rx->commit);
	}
	rx->extended = false;
	memcpy(rx->overhead, msg, 12);
	rx->configured = true;
	if (rx->tcont && rx->serial_valid && !rx->discovering)
		sd5113_discover(rx, ONU_UNASSIGNED);
}

static void sd5113_ploam_received(struct sd5113_gpon_rx *rx, const u8 msg[12])
{
	u32 bits;
	u8 dest = msg[0];

	switch (msg[1]) {
	case 0x01: /* Upstream_Overhead */
		if (dest == ONU_UNASSIGNED && !rx->disabled && rx->onu_id == ONU_UNASSIGNED)
			sd5113_set_overhead(rx, msg);
		break;
	case 0x03: /* Assign_ONU-ID */
		if (dest != ONU_UNASSIGNED || msg[2] > 253 || !rx->configured ||
		    rx->disabled || !rx->serial_valid || memcmp(msg + 3, rx->serial, 8) ||
		    rx->onu_id != ONU_UNASSIGNED)
			break;
		sd5113_write_id(rx, msg[2]);
		if ((readl(rx->id_reg) & 0xff) == msg[2]) {
			rx->onu_id = msg[2];
			if (rx->tcont)
				sd5113_discover(rx, rx->onu_id);
			rx->assign_deadline = jiffies + 10 * HZ;
		} else {
			sd5113_write_id(rx, ONU_UNASSIGNED);
		}
		break;
	case 0x04: /* Ranging_Time, main path */
		if (dest == ONU_UNASSIGNED || dest != rx->onu_id || (msg[2] & BIT(0)))
			break;
		bits = get_unaligned_be32(msg + 3);
		if (!rx->ranged || bits != rx->eqd_bits) {
			sd5113_write_eqd(rx, bits);
			if (readl(rx->eqd_reg) != bits >> 3 ||
			    (readl(rx->eqd_reg + 4) & 7) != (bits & 7)) {
				sd5113_write_eqd(rx, 0);
				sd5113_end_session(rx);
				break;
			}
			rx->eqd_bits = bits;
			rx->ranged = true;
			if (rx->discovering) {
				sd5113_clear_ploam_tx(rx);
				rx->discovering = false;
			}
			rx->omcc_revoked = false;
			if (rx->extended)
				writel(rx->ranged_preamble, rx->burst + 8);
		}
		break;
	case 0x05: /* Deactivate_ONU-ID */
		if (dest == rx->onu_id || dest == ONU_UNASSIGNED) {
			rx->omcc_revoked = true;
			sd5113_end_session(rx);
		}
		break;
	case 0x06: /* Disable_Serial_Number */
		if (dest != ONU_UNASSIGNED || !rx->serial_valid)
			break;
		if (msg[2] == 0x0f) {
			rx->disabled = false;
		} else if (!memcmp(msg + 3, rx->serial, 8)) {
			if (msg[2] == 0xff) {
				sd5113_end_session(rx);
				rx->disabled = true;
				rx->omcc_revoked = true;
			} else if (!msg[2]) {
				rx->disabled = false;
			}
		}
		break;
	case 0x08: /* Encrypted_Port-ID */
		if (rx->crypto && rx->ranged && dest == rx->onu_id)
			sd5113_encrypted_port(rx, msg);
		break;
	case 0x0d: /* Request_Key */
		if (rx->crypto && rx->ranged && dest == rx->onu_id)
			sd5113_request_key(rx);
		break;
	case 0x13: /* Key_Switching_Time: 30-bit downstream superframe */
		if (rx->crypto && rx->ranged && rx->key_sent && dest == rx->onu_id) {
			u32 time = get_unaligned_be32(msg + 2) & GENMASK(29, 0);
			writel((readl(rx->crypto) & GENMASK(31, 30)) | time, rx->crypto);
			writel(readl(rx->crypto + 0x68) | 1, rx->crypto + 0x68);
			sd5113_ack_ploam(rx, msg);
		}
		break;
	case 0x09: /* Request_Password */
		if (rx->ploam_tx && rx->ranged && dest == rx->onu_id &&
		    !rx->disabled && rx->password_valid) {
			u8 reply[12] = { dest, 2 };

			memcpy(reply + 2, rx->password, sizeof(rx->password));
			sd5113_send_ploam(rx, reply, 3);
			memzero_explicit(reply, sizeof(reply));
		}
		break;
	case 0x0a: /* Assign_Alloc-ID */
		if (rx->tcont && rx->ranged && dest == rx->onu_id && !rx->disabled)
			sd5113_assign_alloc(rx, msg);
		break;
	case 0x0e: /* Configure_Port-ID */
		if (rx->gem_table && rx->ranged && dest == rx->onu_id && !rx->disabled)
			sd5113_configure_omcc(rx, msg);
		break;
	case 0x14: /* Extended_Burst_Length */
		if (dest == ONU_UNASSIGNED && rx->configured &&
		    rx->overhead[3] + rx->overhead[4] + 8 * max(msg[2], msg[3]) <= 968) {
			rx->extended = true;
			rx->ranged_preamble = msg[3];
			writel(rx->ranged ? msg[3] : msg[2], rx->burst + 8);
		}
		break;
	}
}

/* GLB+0x10 is W1C. The next register masks PLOAM on bit 0 and
 * Ethernet on bit 4. Both drivers serialize mask updates via syscon.
 */
#define PLOAM_IRQ_CAUSE 0
#define PLOAM_IRQ_MASK  4

static irqreturn_t sd5113_ploam_irq(int irq, void *data)
{
	struct sd5113_gpon_rx *rx = data;
	u32 cause, mask;

	if (regmap_read(rx->irq_regs, PLOAM_IRQ_CAUSE, &cause) ||
	    regmap_read(rx->irq_regs, PLOAM_IRQ_MASK, &mask) ||
	    !(cause & ~mask & BIT(0)))
		return IRQ_NONE;
	regmap_set_bits(rx->irq_regs, PLOAM_IRQ_MASK, BIT(0));
	regmap_write(rx->irq_regs, PLOAM_IRQ_CAUSE, BIT(0));
	mod_delayed_work(system_highpri_wq, &rx->drain, 0);
	return IRQ_HANDLED;
}

/* The downstream mailbox is 64 words, with network octets MSB first. */
static void drain_omci(struct sd5113_gpon_rx *rx)
{
	unsigned int budget, i;

	for (budget = 0; budget < OMCI_QUEUE_SIZE && (readl(rx->omci_ready) & 1); budget++) {
		u8 *slot = rx->omci_slots[(rx->head + rx->count) % OMCI_QUEUE_SIZE];

		if (rx->count == OMCI_QUEUE_SIZE) {
			rx->dropped++;
		} else {
			for (i = 0; i < OMCI_SLOT_SIZE; i += 4)
				put_unaligned_be32(readl(rx->omci_data + i), slot + i);
			rx->count++;
		}
		rx->omcc_seen = true;
		rx->received++;
		writel(readl(rx->omci_ready) & ~BIT(0), rx->omci_ready);
	}
}

static ssize_t omci_frame_read(struct file *file, struct kobject *kobj,
			      struct bin_attribute *attr, char *buf, loff_t off, size_t len)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(kobj_to_dev(kobj));
	ssize_t ret;

	if (off)
		return 0;
	if (len < OMCI_SLOT_SIZE)
		return -EMSGSIZE;
	mutex_lock(&rx->lock);
	if (!rx->capture || !rx->count) {
		ret = rx->capture ? -EAGAIN : -EHOSTDOWN;
	} else {
		memcpy(buf, rx->omci_slots[rx->head], OMCI_SLOT_SIZE);
		rx->head = (rx->head + 1) % OMCI_QUEUE_SIZE;
		rx->count--;
		ret = OMCI_SLOT_SIZE;
	}
	mutex_unlock(&rx->lock);
	return ret;
}
/* Baseline OMCI body; the MAC supplies the trailer and transmits on its
 * provisioned channel. On this board the OLT assigns the management GEM
 * Port-ID via Configure_Port-ID after ranging.
 */
static ssize_t omci_tx_write(struct file *file, struct kobject *kobj,
			     struct bin_attribute *attr, char *buf, loff_t off, size_t len)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(kobj_to_dev(kobj));
	unsigned int i;
	ssize_t ret;

	if (off || (len != 40 && len != 44) || buf[len - 37] != 0x0a)
		return -EINVAL;
	mutex_lock(&rx->lock);
	if (len == 44 && get_unaligned_be32(buf) != rx->session) {
		ret = -ESTALE;
	} else if (!rx->capture || rx->disabled || (rx->gem_table ? rx->omcc_port < 0 : !rx->omcc_seen) || (readl(rx->los) & 1) || (readl(rx->lof) & 2)) {
		ret = -ENOLINK;
	} else if (readl(rx->omci_send) & 1) {
		ret = -EAGAIN;
	} else {
		for (i = 0; i < 40; i += 4)
			writel(get_unaligned_be32(buf + len - 40 + i), rx->omci_out + i);
		writel(40, rx->omci_out + 40);
		wmb();
		writel(readl(rx->omci_send) | 1, rx->omci_send);
		ret = len;
	}
	mutex_unlock(&rx->lock);
	return ret;
}
static struct bin_attribute bin_attr_omci_tx = __BIN_ATTR(omci_tx, 0200, NULL, omci_tx_write, 0);

static struct bin_attribute bin_attr_omci_frame = __BIN_ATTR(omci_frame, 0400, omci_frame_read, NULL, OMCI_SLOT_SIZE);

static ssize_t omci_capture_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(rx->capture));
}

static ssize_t omci_capture_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	mutex_lock(&rx->lock);
	rx->capture = enable;
	if (!enable) {
		rx->count = rx->head = 0;
		rx->omcc_seen = false;
	}
	if (!rx->stopping)
		mod_delayed_work(system_highpri_wq, &rx->drain, 0);
	mutex_unlock(&rx->lock);
	return len;
}
static DEVICE_ATTR_RW(omci_capture);

static ssize_t omci_received_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(rx->received));
}
static DEVICE_ATTR_RO(omci_received);

static ssize_t omci_dropped_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(rx->dropped));
}
static DEVICE_ATTR_RO(omci_dropped);

static struct attribute *omci_attrs[] = {
	&dev_attr_omci_capture.attr, &dev_attr_omci_received.attr, &dev_attr_omci_dropped.attr, NULL
};
static struct bin_attribute *omci_bin_attrs[] = { &bin_attr_omci_frame, &bin_attr_omci_tx, NULL };
static const struct attribute_group omci_group = { .attrs = omci_attrs, .bin_attrs = omci_bin_attrs };

static void sd5113_drain(struct work_struct *work)
{
	struct sd5113_gpon_rx *rx = container_of(to_delayed_work(work),
						struct sd5113_gpon_rx, drain);
	bool loss;
	unsigned int i, n;
	u8 msg[12];

	mutex_lock(&rx->lock);
	loss = (readl(rx->los) & BIT(0)) || (readl(rx->lof) & BIT(1));
	if (loss || ((rx->discovering || rx->onu_id != ONU_UNASSIGNED) && !rx->ranged &&
		     time_after_eq(jiffies, rx->assign_deadline)))
		sd5113_end_session(rx);
	for (n = 0; n < 64 && (readl(rx->ploam_pending) & BIT(0)); n++) {
		for (i = 0; i < 3; i++)
			put_unaligned_be32(readl(rx->frame + i * 4), msg + i * 4);
		writel(readl(rx->ploam_pending) & ~BIT(0), rx->ploam_pending);
		if (!loss)
			sd5113_ploam_received(rx, msg);
	}
	if (rx->capture) {
		if (loss || rx->disabled || rx->omcc_revoked || (rx->gem_table && rx->omcc_port < 0)) {
			u32 ready = readl(rx->omci_ready);

			if (ready & BIT(0))
				writel(ready & ~BIT(0), rx->omci_ready);
		} else {
			drain_omci(rx);
		}
	}
	if (!rx->stopping) {
		if (n < 64)
			regmap_clear_bits(rx->irq_regs, PLOAM_IRQ_MASK, BIT(0));
		/* Revisit LOS/LOF even when the optical link is idle. */
		queue_delayed_work(system_highpri_wq, &rx->drain, n == 64 ? 0 : msecs_to_jiffies(rx->capture && !loss ? 2 : 50));
	}
	mutex_unlock(&rx->lock);
}

static ssize_t upstream_overhead_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&rx->lock);
	if (rx->configured)
		len = sysfs_emit(buf, "%*phN\n", 10, rx->overhead + 2);
	else
		len = sysfs_emit(buf, "waiting\n");
	mutex_unlock(&rx->lock);
	return len;
}
static DEVICE_ATTR_RO(upstream_overhead);

static ssize_t factory_serial_status_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", rx->serial_valid ? "ready" : "unavailable");
}
static DEVICE_ATTR_RO(factory_serial_status);

static ssize_t onu_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&rx->lock);
	if (rx->disabled)
		len = sysfs_emit(buf, "disabled\n");
	else if (rx->onu_id == ONU_UNASSIGNED)
		len = sysfs_emit(buf, "unassigned\n");
	else
		len = sysfs_emit(buf, "%u\n", rx->onu_id);
	mutex_unlock(&rx->lock);
	return len;
}
static DEVICE_ATTR_RO(onu_id);

static ssize_t equalization_delay_bits_show(struct device *dev, struct device_attribute *attr,
					   char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&rx->lock);
	len = rx->ranged ? sysfs_emit(buf, "%u\n", rx->eqd_bits) : sysfs_emit(buf, "waiting\n");
	mutex_unlock(&rx->lock);
	return len;
}
static DEVICE_ATTR_RO(equalization_delay_bits);

static ssize_t alloc_ids_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	int i, n = 0;

	mutex_lock(&rx->lock);
	for (i = 0; i < 8; i++)
		if (rx->owned_tcont & BIT(i))
			n += sysfs_emit_at(buf, n, "%s%u", n ? " " : "",
					   (readl(rx->tcont + 4 * i) >> 4) & 0xfff);
	mutex_unlock(&rx->lock);
	return n + sysfs_emit_at(buf, n, "\n");
}
static DEVICE_ATTR_RO(alloc_ids);

static ssize_t ploam_tx_busy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(rx->ploam_tx_busy));
}
static DEVICE_ATTR_RO(ploam_tx_busy);

static ssize_t omcc_port_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	int port = READ_ONCE(rx->omcc_port);

	return port < 0 ? sysfs_emit(buf, "unassigned\n") : sysfs_emit(buf, "%d\n", port);
}
static DEVICE_ATTR_RO(omcc_port);

static ssize_t omci_session_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u %d\n", READ_ONCE(rx->session), READ_ONCE(rx->omcc_port));
}
static DEVICE_ATTR_RO(omci_session);

static ssize_t onu_serial_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return rx->serial_valid ? sysfs_emit(buf, "%8phN\n", rx->serial) : -ENODATA;
}
static struct device_attribute dev_attr_onu_serial = __ATTR(onu_serial, 0400, onu_serial_show, NULL);

static int sd5113_apply_plan(struct sd5113_gpon_rx *rx, const struct sd5113_plan *plan)
{
	struct sd5113_admitted_gem next[8];
	struct sd5113_gem_profile profile = {};
	unsigned int p, n = 0, gem, alloc, slot, i;
	unsigned int omcc = rx->omcc_port;
	int old, ret;
	u32 before[8];

	if (rx->admitted && !memcmp(&rx->plan, plan, sizeof(*plan)))
		return 0;
	for (p = 0; p < ARRAY_SIZE(plan->paths); p++) {
		u32 path = plan->paths[p];

		if (path == U32_MAX)
			continue;
		gem = path >> 12;
		alloc = path & 4095;
		if (gem >= 4095 || gem == omcc || alloc == 4095)
			return -EINVAL;
		for (slot = 0; slot < 8; slot++) {
			u32 t = readl(rx->tcont + 4 * slot);

			if ((rx->owned_tcont & BIT(slot)) && (t & BIT(31)) && ((t >> 4) & 4095) == alloc)
				break;
		}
		if (slot == 8)
			return -EAGAIN;
		profile.route[p] = BIT(31) | ((u32)plan->vid << 19) | (slot << 12) | gem;
		if (sd5113_data_index(next, n, gem) >= 0)
			continue;
		old = sd5113_data_index(rx->data, rx->admitted, gem);
		next[n].id = gem;
		before[n] = readl(rx->gem_table + 4 * gem);
		next[n].saved = old < 0 ? before[n] : rx->data[old].saved;
		n++;
	}
	if (!n) {
		sd5113_clear_data(rx);
		return 0;
	}
	/* RX descriptors identify PON ingress. All admitted paths must terminate
	 * on the same VEIP with the same downstream VLAN treatment.
	 */
	for (i = 0; i < 4096; i++)
		if (i != omcc && sd5113_data_index(rx->data, rx->admitted, i) < 0 &&
		    (readl(rx->gem_table + 4 * i) & BIT(4)))
			return -EBUSY;
	memcpy(profile.filters, plan->filters, sizeof(profile.filters));
	profile.tag_pcp = plan->tag_pcp;
	profile.unmarked = plan->unmarked;
	for (i = 0; i < 64; i++) {
		unsigned int bit = i * 3, byte = bit / 8;
		u16 window = (u16)plan->dscp[byte] << 8;

		if (byte + 1 < sizeof(plan->dscp))
			window |= plan->dscp[byte + 1];
		profile.dscp[i] = (window >> (13 - bit % 8)) & 7;
	}
	for (i = 0; i < n; i++) {
		void __iomem *entry = rx->gem_table + 4 * next[i].id;
		u32 v = (before[i] & ~0xffU) | BIT(4);

		if (test_bit(next[i].id, rx->encrypted))
			v |= BIT(3);
		writel(v, entry);
		if (readl(entry) != v) {
			ret = -EIO;
			goto restore;
		}
	}
	ret = sd5113_eth_set_gem(&profile);
	if (ret)
		goto restore;
	for (i = 0; i < rx->admitted; i++)
		if (sd5113_data_index(next, n, rx->data[i].id) < 0) {
			void __iomem *entry = rx->gem_table + 4 * rx->data[i].id;

			writel(rx->data[i].saved, entry);
			readl(entry);
		}
	memcpy(rx->data, next, n * sizeof(*next));
	rx->admitted = n;
	rx->plan = *plan;
	return 0;
restore:
	for (i = 0; i < n; i++) {
		void __iomem *entry = rx->gem_table + 4 * next[i].id;

		writel(before[i], entry);
		readl(entry);
	}
	return ret;
}

static ssize_t gem_service_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	ssize_t n = 0;
	int p;
	bool uniform = true;

	mutex_lock(&rx->lock);
	for (p = 1; p < 8; p++)
		if (rx->plan.paths[p] != rx->plan.paths[0])
			uniform = false;
	if (!rx->admitted)
		n = sysfs_emit(buf, "unassigned\n");
	else if (uniform)
		n = sysfs_emit(buf, "gem=%u alloc=%u vid=%u\n", rx->plan.paths[0] >> 12,
			       rx->plan.paths[0] & 4095, rx->plan.vid);
	else {
		n = sysfs_emit(buf, "vid=%u", rx->plan.vid);
		for (p = 0; p < 8; p++)
			if (rx->plan.paths[p] != U32_MAX)
				n += sysfs_emit_at(buf, n, " p%d=%u/%u", p,
						   rx->plan.paths[p] >> 12, rx->plan.paths[p] & 4095);
		n += sysfs_emit_at(buf, n, "\n");
	}
	mutex_unlock(&rx->lock);
	return n;
}

static ssize_t gem_service_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);
	struct sd5113_plan plan = {};
	unsigned int epoch, gem, alloc, vid, tag, mode, i;
	char hex[49], filters[105], extra;
	int fields;
	int ret = 0;

	mutex_lock(&rx->lock);
	if (sysfs_streq(buf, "off")) {
		sd5113_clear_data(rx);
		goto out;
	}
	if (!strncmp(buf, "map ", 4)) {
		u32 *v = plan.paths;

		fields = sscanf(buf, "map %u %u %u %u %48s %x %x %x %x %x %x %x %x %104s %c",
			   &epoch, &vid, &tag, &mode, hex, &v[0], &v[1], &v[2],
			   &v[3], &v[4], &v[5], &v[6], &v[7], filters, &extra);
		if ((fields != 13 && fields != 14) ||
		    strlen(hex) != 48 || hex2bin(plan.dscp, hex, 24) || tag > 7 || mode > 8) {
			ret = -EINVAL;
			goto out;
		}
		if (fields == 14) {
			if (strlen(filters) != 104 || hex2bin((u8 *)plan.filters, filters, sizeof(plan.filters))) {
				ret = -EINVAL;
				goto out;
			}
			for (i = 0; i < 2; i++) {
				u8 op = plan.filters[i][0];

				if (plan.filters[i][1] > 12 || (op > 2 && (op < 15 || op > 20))) {
					ret = -EINVAL;
					goto out;
				}
			}
		}
		plan.tag_pcp = tag;
		plan.unmarked = mode;
	} else {
		if (sscanf(buf, "%u %u %u %u %c", &epoch, &gem, &alloc, &vid, &extra) != 4 ||
		    gem >= 4095 || alloc >= 4095) {
			ret = -EINVAL;
			goto out;
		}
		for (i = 0; i < 8; i++)
			plan.paths[i] = gem << 12 | alloc;
	}
	if (vid > 4094) {
		ret = -EINVAL;
		goto out;
	}
	plan.vid = vid;
	if (!rx->gem_table || rx->omcc_port < 0 || !rx->ranged || rx->session != epoch ||
	    rx->stopping || (readl(rx->los) & 1) || (readl(rx->lof) & 2))
		ret = -ENOLINK;
	else
		ret = sd5113_apply_plan(rx, &plan);
out:
	mutex_unlock(&rx->lock);
	return ret ? ret : len;
}
static DEVICE_ATTR_RW(gem_service);

static struct attribute *sd5113_burst_attrs[] = {
	&dev_attr_gem_service.attr, &dev_attr_omci_session.attr, &dev_attr_onu_serial.attr,
	&dev_attr_omcc_port.attr, &dev_attr_alloc_ids.attr, &dev_attr_ploam_tx_busy.attr,
	&dev_attr_upstream_overhead.attr, &dev_attr_factory_serial_status.attr,
	&dev_attr_onu_id.attr, &dev_attr_equalization_delay_bits.attr, NULL
};
ATTRIBUTE_GROUPS(sd5113_burst);

static void sd5113_stop_drain(void *data)
{
	struct sd5113_gpon_rx *rx = data;

	mutex_lock(&rx->lock);
	rx->stopping = true;
	regmap_set_bits(rx->irq_regs, PLOAM_IRQ_MASK, BIT(0));
	mutex_unlock(&rx->lock);
	free_irq(rx->irq, rx);
	cancel_delayed_work_sync(&rx->drain);
	mutex_lock(&rx->lock);
	sd5113_end_session(rx);
	memzero_explicit(rx->password, sizeof(rx->password));
	mutex_unlock(&rx->lock);
	regmap_update_bits(rx->irq_regs, PLOAM_IRQ_MASK, BIT(0), rx->old_mask);
}

static int sd5113_burst_probe(struct platform_device *pdev, struct sd5113_gpon_rx *rx)
{
	int ret;

	rx->frame = devm_platform_ioremap_resource_byname(pdev, "ploam-data");
	if (IS_ERR(rx->frame))
		return PTR_ERR(rx->frame);
	rx->pregap = devm_platform_ioremap_resource_byname(pdev, "pre-eqd");
	if (IS_ERR(rx->pregap))
		return PTR_ERR(rx->pregap);
	rx->commit = devm_platform_ioremap_resource_byname(pdev, "timing-commit");
	if (IS_ERR(rx->commit))
		return PTR_ERR(rx->commit);
	rx->burst = devm_platform_ioremap_resource_byname(pdev, "burst-format");
	if (IS_ERR(rx->burst))
		return PTR_ERR(rx->burst);

	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "omci-ready")) {
		rx->omci_ready = devm_platform_ioremap_resource_byname(pdev, "omci-ready");
		if (IS_ERR(rx->omci_ready))
			return PTR_ERR(rx->omci_ready);
		rx->omci_data = devm_platform_ioremap_resource_byname(pdev, "omci-data");
		if (IS_ERR(rx->omci_data))
			return PTR_ERR(rx->omci_data);
		rx->omci_send = devm_platform_ioremap_resource_byname(pdev, "omci-send");
		if (IS_ERR(rx->omci_send))
			return PTR_ERR(rx->omci_send);
		rx->omci_out = devm_platform_ioremap_resource_byname(pdev, "omci-out");
		if (IS_ERR(rx->omci_out))
			return PTR_ERR(rx->omci_out);
	}
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "gem-crypto")) {
		rx->crypto = devm_platform_ioremap_resource_byname(pdev, "gem-crypto");
		if (IS_ERR(rx->crypto))
			return PTR_ERR(rx->crypto);
	}
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "downstream-gem")) {
		rx->gem_table = devm_platform_ioremap_resource_byname(pdev, "downstream-gem");
		if (IS_ERR(rx->gem_table))
			return PTR_ERR(rx->gem_table);
		rx->omci_route = devm_platform_ioremap_resource_byname(pdev, "omci-route");
		if (IS_ERR(rx->omci_route))
			return PTR_ERR(rx->omci_route);
		rx->ploam_tx = devm_platform_ioremap_resource_byname(pdev, "ploam-tx");
		if (IS_ERR(rx->ploam_tx))
			return PTR_ERR(rx->ploam_tx);
		rx->tcont = devm_platform_ioremap_resource_byname(pdev, "tcont");
		if (IS_ERR(rx->tcont))
			return PTR_ERR(rx->tcont);
		rx->random_delay = devm_platform_ioremap_resource_byname(pdev, "random-delay");
		if (IS_ERR(rx->random_delay))
			return PTR_ERR(rx->random_delay);
	}
	ret = sd5113_activation_probe(pdev, rx);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "activation setup failed\n");
	ret = sd5113_phy_start(pdev, rx);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "GPON PHY setup failed\n");
	mutex_init(&rx->lock);
	INIT_DELAYED_WORK(&rx->drain, sd5113_drain);
	ret = devm_device_add_groups(&pdev->dev, sd5113_burst_groups);
	if (ret)
		return ret;
	rx->irq_regs = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "hisilicon,dp-irq-regs");
	if (IS_ERR(rx->irq_regs))
		return PTR_ERR(rx->irq_regs);
	rx->irq = platform_get_irq(pdev, 0);
	if (rx->irq < 0)
		return rx->irq;
	ret = regmap_read(rx->irq_regs, PLOAM_IRQ_MASK, &rx->old_mask);
	if (ret)
		return ret;
	ret = request_irq(rx->irq, sd5113_ploam_irq, IRQF_SHARED, dev_name(&pdev->dev), rx);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, sd5113_stop_drain, rx);
	if (!ret)
		mod_delayed_work(system_highpri_wq, &rx->drain, 0);
	if (!ret && rx->omci_data)
		ret = devm_device_add_group(&pdev->dev, &omci_group);
	return ret;
}

static ssize_t los_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(rx->los) & 1U);
}
static DEVICE_ATTR_RO(los);

static ssize_t lof_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", (readl(rx->lof) >> 1) & 1U);
}
static DEVICE_ATTR_RO(lof);

static ssize_t ploam_rx_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(rx->ploam_rx));
}
static DEVICE_ATTR_RO(ploam_rx);

static ssize_t ploam_crc_errors_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(rx->ploam_crc_errors));
}
static DEVICE_ATTR_RO(ploam_crc_errors);

static ssize_t ploam_pending_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(rx->ploam_pending) & 1U);
}
static DEVICE_ATTR_RO(ploam_pending);

static ssize_t ploam_dropped_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct sd5113_gpon_rx *rx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(rx->ploam_dropped));
}
static DEVICE_ATTR_RO(ploam_dropped);

static struct attribute *sd5113_queue_attrs[] = {
	&dev_attr_ploam_pending.attr,
	&dev_attr_ploam_dropped.attr,
	NULL
};
ATTRIBUTE_GROUPS(sd5113_queue);

static struct attribute *sd5113_rx_attrs[] = {
	&dev_attr_los.attr,
	&dev_attr_lof.attr,
	&dev_attr_ploam_rx.attr,
	&dev_attr_ploam_crc_errors.attr,
	NULL
};
ATTRIBUTE_GROUPS(sd5113_rx);

static int sd5113_gpon_rx_probe(struct platform_device *pdev)
{
	struct sd5113_gpon_rx *rx;
	int ret;

	rx = devm_kzalloc(&pdev->dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;
	rx->los = devm_platform_ioremap_resource_byname(pdev, "los");
	if (IS_ERR(rx->los))
		return PTR_ERR(rx->los);
	rx->lof = devm_platform_ioremap_resource_byname(pdev, "lof");
	if (IS_ERR(rx->lof))
		return PTR_ERR(rx->lof);
	rx->ploam_rx = devm_platform_ioremap_resource_byname(pdev, "ploam-rx");
	if (IS_ERR(rx->ploam_rx))
		return PTR_ERR(rx->ploam_rx);
	rx->ploam_crc_errors = devm_platform_ioremap_resource_byname(pdev, "ploam-crc-errors");
	if (IS_ERR(rx->ploam_crc_errors))
		return PTR_ERR(rx->ploam_crc_errors);
	platform_set_drvdata(pdev, rx);
	if (of_device_is_compatible(pdev->dev.of_node, "hisilicon,sd5113-gpon-rx-queue")) {
		rx->ploam_pending = devm_platform_ioremap_resource_byname(pdev, "ploam-pending");
		if (IS_ERR(rx->ploam_pending))
			return PTR_ERR(rx->ploam_pending);
		rx->ploam_dropped = devm_platform_ioremap_resource_byname(pdev, "ploam-dropped");
		if (IS_ERR(rx->ploam_dropped))
			return PTR_ERR(rx->ploam_dropped);
		ret = devm_device_add_groups(&pdev->dev, sd5113_queue_groups);
		if (ret)
			return ret;
		if (of_device_is_compatible(pdev->dev.of_node, "hisilicon,sd5113-gpon-burst"))
			return sd5113_burst_probe(pdev, rx);
	}
	return 0;
}

static const struct of_device_id sd5113_gpon_rx_ids[] = {
	{ .compatible = "hisilicon,sd5113-gpon-rx-status" },
	{ }
};

static struct platform_driver sd5113_gpon_rx_driver = {
	.probe = sd5113_gpon_rx_probe,
	.driver = {
		.name = "sd5113-gpon-rx",
		.of_match_table = sd5113_gpon_rx_ids,
		.dev_groups = sd5113_rx_groups,
	},
};
module_platform_driver(sd5113_gpon_rx_driver);

MODULE_DEVICE_TABLE(of, sd5113_gpon_rx_ids);
MODULE_DESCRIPTION("SD5113 GPON downstream PLOAM and upstream burst framing");
MODULE_LICENSE("GPL");

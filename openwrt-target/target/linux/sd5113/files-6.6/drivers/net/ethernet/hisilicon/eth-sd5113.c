// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 "CPU port" Ethernet MAC driver (HG8245 reverse-engineered BSP) */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bitfield.h>
#include <linux/sd5113-lan.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/netdevice.h>
#include <linux/sd5113-gem.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/if_vlan.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>
#include <asm/unaligned.h>

#define DRV_NAME "sd5113-eth"

/* ---------------------------------------------------------------------
 * g_glb_register block - physical 0x42000010, size 8 bytes. CONFIRMED.
 * ------------------------------------------------------------------- */
#define GLB_IRQ_CAUSE		0x00	/* bit 4: RX interrupt latch, W1C */
#define GLB_IRQ_MASK		0x04	/* CONFIRMED: bit4 = eth event, RMW, mask-on-service */
#define GLB_ETH_IRQ_BIT		BIT(4)
#define GLB_CPU_VLAN_TPID	0x0c
#define GLB_CPU_PORT_CFG	0xb8

#define FE_CPU_CTRL		0x00
#define FE_CPU_FILTER		0x38
/* CAP has one membership word per VID. MCD heads are indexed by VID,
 * followed by a pool of linked destination records.
 */
#define CAP_MEMBER_MASK		GENMASK(6, 0)
#define FE_CPU_PORT		6
#define MCD_TAIL_BASE		VLAN_N_VID
#define MCD_TAIL_COUNT		512
#define MCD_WORDS		(MCD_TAIL_BASE + MCD_TAIL_COUNT)
#define MCD_VALID		BIT(25)
#define MCD_HAS_NEXT		BIT(24)
#define MCD_PORT		GENMASK(15, 13)
#define MCD_NEXT		GENMASK(12, 0)
#define MCD_FIELDS		GENMASK(25, 0)
#define SD5113_FE_CPU_VID	1
#define SD5113_FE_TX_CTRL1	4

/* ---------------------------------------------------------------------
 * g_cpurtx_register block - physical 0x42100000, size 0xd8 bytes.
 * ------------------------------------------------------------------- */
#define CPURTX_TX_KICK		0x00	/* CONFIRMED: bit0 = kick, write 1 */
#define CPURTX_TX_PENDING	BIT(0)
#define CPURTX_TX_CTRL0		0x04	/* CONFIRMED (round 30): in the
					 * desc[6]==0 ("no VLAN") path, holds
					 * the 11-bit length duplicated at bit
					 * positions [14:4] and [31:21] - see
					 * NOTES_ETH.md round 30. Medium-high
					 * confidence, not 100%. */
#define CPURTX_TX_BUFADDR	0x08	 
#define CPURTX_TX_CTRL1		0x0c
#define CPURTX_TX_STATUS	0x10	/* CONFIRMED: bit0 = TX completion pending */
#define CPURTX_TX_RESULT	0x14	/* CONFIRMED: completion returns the
					 * device-domain TX buffer address */
#define CPURTX_RX_SUBMIT_STATE	0x18	/* CONFIRMED (round 30, eth_prepare_rx_buffer):
					 * bit0 = RX submission slot busy/free */
#define CPURTX_RX_SUBMIT_ADDR	0x1c	/* CONFIRMED (round 30): device-domain
					 * address of an RX buffer being handed
					 * to hardware BEFORE it receives
					 * anything - this is buffer submission,
					 * not a post-receive readout register. */
#define CPURTX_FIFO_RESET	0x20	/* CONFIRMED: bit0 = reset, self-clears */
#define CPURTX_CAUSE		0x30	/* CONFIRMED: bits 0-7 = 8 RX event
					 * slots pending (round 29 said 6 - that
					 * was an incomplete read of the
					 * function, corrected in round 30).
					 * Ack by writing the bitwise inverse of
					 * the handled bit (confirmed-map.json). */
#define CPURTX_MTU_CTRL		0x94
#define CPURTX_MTU_COUNT	0x98
#define CPURTX_RXFIFO_ALARM	0xd4

#define CPURTX_REGION_SIZE	0xd8

/* CONFIRMED (round 30, corrects round 29's "6 slots"): 8 RX descriptor
 * slots, 12 bytes each, one per CPURTX_CAUSE bit, DESCENDING bit->offset,
 * base 0x34, ending exactly at CPURTX_MTU_CTRL (0x94) with no gap. */
#define SD5113_RX_NUM_SLOTS	8
#define SD5113_RX_SLOT_MASK	GENMASK(SD5113_RX_NUM_SLOTS - 1, 0)
#define SD5113_RX_SLOT_BASE(bit)	(0x88 - 0x0c * (bit))
#define SD5113_RX_SLOT_CTRL(bit)	(SD5113_RX_SLOT_BASE(bit) + 0x00)
#define SD5113_RX_SLOT_WORD1(bit)	(SD5113_RX_SLOT_BASE(bit) + 0x04)
#define SD5113_RX_SLOT_TAG(bit)		(SD5113_RX_SLOT_BASE(bit) + 0x08)

#define RX_CTRL_CATEGORY(ctrl)	((ctrl) & 0x7)
#define RX_CTRL_LEN(ctrl)	(((ctrl) >> 9) & 0x7ff)
#define RX_CTRL_EXTRA(ctrl)	(((ctrl) >> 20) & 0x7)

/* CONFIRMED (round 30, eth_special_process init loop, hi_eth.dis 0xe7c-0xf00):
 * exactly 128 RX buffers are pre-allocated and submitted at init. The
 * 7-bit recovery index (round 30: byte at word1-4, masked &0x7f) requires
 * this exact count - do not change without re-checking that mask. */
#define SD5113_RX_POOL_SIZE	128
#define SD5113_RX_BUF_SIZE	0x7e0
#define SD5113_TX_FRAME_MAX	(VLAN_ETH_FRAME_LEN)
#define SD5113_SERVICE_MS	10
#define SD5113_TX_MAX_OUTSTANDING	9	/* CONFIRMED: vendor g_tx_send_max=9 */

#define SD5113_POLL_TIMEOUT_US	5000	/* this driver's own choice, not
					 * extracted from vendor code, which
					 * loops with a fixed iteration count
					 * instead of a real timeout. */

struct sd5113_rx_buf {
	struct sk_buff	*skb;
	dma_addr_t	dma_addr;	/* device-domain address as submitted
					 * to CPURTX_RX_SUBMIT_ADDR */
};

struct sd5113_tx_buf {
	struct sk_buff	*skb;
	dma_addr_t	dma_addr;
};

struct sd5113_fe {
	void __iomem *port_cfg;
	void __iomem *cpu, *classifier, *policy, *dest, *gpio;
	u32 __iomem *members, *broadcast;
	u32 trunk_port;
	u16 bc_tail;
	bool bc_tail_owned;
	u32 saved_tail;
	struct {
		u32 members, head;
	} saved_lan[SD5113_LAN_PORT_COUNT];
	u32 saved_cpu, saved_filter, saved_mode, saved_tpid, saved_port_cfg;
};

static DEFINE_MUTEX(sd5113_port_lock);
static struct sd5113_eth *sd5113_pon_mac;

struct sd5113_eth {
	struct net_device *optic;
	u32 pon_classifier[8][8], pon_policy[32], pon_dest[32];
	struct sd5113_gem_profile qos;
	u32 gem_route; /* BIT(31), VID[30:19], TCONT[14:12], GEM[11:0] */
	struct regmap *irq_regs;
	struct net_device	*ndev;
	struct device		*dev;
	void __iomem		*glb;		/* 0x42000010 */
	void __iomem		*cpurtx;	/* 0x42100000, 0xd8 bytes */
	int			irq;
	struct napi_struct	napi;
	spinlock_t		lock;
	struct delayed_work	service_work;
	bool			running;
	struct sd5113_fe	*fe;

	struct sd5113_tx_buf	tx_buf[SD5113_TX_MAX_OUTSTANDING];
	unsigned int		tx_count;
	atomic_t		dma_tx_map_logged;
	atomic_t		dma_tx_done_logged;
	atomic_t		dma_rx_map_logged;
	atomic_t		dma_rx_done_logged;

	struct sd5113_rx_buf	rx_pool[SD5113_RX_POOL_SIZE];
};

/* ---------------------------------------------------------------------
 * RX buffer submission (CONFIRMED register sequence, eth_prepare_rx_buffer)
 * ------------------------------------------------------------------- */

static int sd5113_eth_submit_rx_buf(struct sd5113_eth *priv, dma_addr_t addr)
{
	u32 val;
	int ret;

	/* CONFIRMED: wait for the single submission slot to be free. */
	ret = readl_poll_timeout_atomic(priv->cpurtx + CPURTX_RX_SUBMIT_STATE,
					 val, !(val & BIT(0)), 10,
					 SD5113_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	/* CONFIRMED: address first, then mark the slot busy. */
	writel((u32)addr, priv->cpurtx + CPURTX_RX_SUBMIT_ADDR);
	writel(readl(priv->cpurtx + CPURTX_RX_SUBMIT_STATE) | BIT(0),
	       priv->cpurtx + CPURTX_RX_SUBMIT_STATE);
	return 0;
}

static int sd5113_eth_fill_rx_slot(struct sd5113_eth *priv, unsigned int idx)
{
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	int ret;

	/* Do not add NET_IP_ALIGN to the device-facing DMA address. */
	skb = netdev_alloc_skb(priv->ndev, SD5113_RX_BUF_SIZE);
	if (!skb)
		return -ENOMEM;

	dma_addr = dma_map_single(priv->dev, skb->data, SD5113_RX_BUF_SIZE,
				   DMA_FROM_DEVICE);
	if (dma_mapping_error(priv->dev, dma_addr)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	if (atomic_cmpxchg(&priv->dma_rx_map_logged, 0, 1) == 0)
		dev_info(priv->dev, "first RX DMA mapping: dma=%pad len=%u\n",
			 &dma_addr, SD5113_RX_BUF_SIZE);

	ret = sd5113_eth_submit_rx_buf(priv, dma_addr);
	if (ret) {
		dma_unmap_single(priv->dev, dma_addr, SD5113_RX_BUF_SIZE,
				  DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
		return ret;
	}

	priv->rx_pool[idx].skb = skb;
	priv->rx_pool[idx].dma_addr = dma_addr;
	return 0;
}

/* CONFIRMED (round 30, eth_special_process init loop): 128 buffers
 * submitted up front before the interface can receive anything. */
static int sd5113_eth_fill_rx_pool(struct sd5113_eth *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < SD5113_RX_POOL_SIZE; i++) {
		if (priv->rx_pool[i].skb)
			continue;
		ret = sd5113_eth_fill_rx_slot(priv, i);
		if (ret)
			return ret;
	}
	return 0;
}

static void sd5113_eth_drain_rx_pool(struct sd5113_eth *priv)
{
	unsigned int i;

	for (i = 0; i < SD5113_RX_POOL_SIZE; i++) {
		if (!priv->rx_pool[i].skb)
			continue;
		dma_unmap_single(priv->dev, priv->rx_pool[i].dma_addr,
				  SD5113_RX_BUF_SIZE, DMA_FROM_DEVICE);
		dev_kfree_skb_any(priv->rx_pool[i].skb);
		priv->rx_pool[i].skb = NULL;
	}
}

/* HYPOTHESIS (deliberate deviation, see struct comment): find which pool
 * entry hardware just echoed back, by matching word1 against what we
 * ourselves submitted - instead of the vendor's raw (word1-4) byte read. */
static int sd5113_eth_find_rx_buf(struct sd5113_eth *priv, u32 word1)
{
	unsigned int i;

	for (i = 0; i < SD5113_RX_POOL_SIZE; i++) {
		if (priv->rx_pool[i].skb &&
		    (u32)priv->rx_pool[i].dma_addr == word1)
			return i;
	}
	return -1;
}

/* ---------------------------------------------------------------------
 * TX
 * ------------------------------------------------------------------- */

static int sd5113_eth_tx_find(struct sd5113_eth *priv, u32 hw_addr)
{
	unsigned int i;

	for (i = 0; i < SD5113_TX_MAX_OUTSTANDING; i++) {
		if (priv->tx_buf[i].skb &&
		    (u32)priv->tx_buf[i].dma_addr == hw_addr)
			return i;
	}

	return -1;
}

static unsigned int
sd5113_eth_tx_reclaim_locked(struct sd5113_eth *priv,
				     struct sd5113_tx_buf *done,
				     unsigned int done_max)
{
	unsigned int done_count = 0;
	unsigned int i;

	for (i = 0; i < SD5113_TX_MAX_OUTSTANDING; i++) {
		u32 status, hw_addr;
		int slot;

		status = readl(priv->cpurtx + CPURTX_TX_STATUS);
		if (!(status & BIT(0)))
			break;

		hw_addr = readl(priv->cpurtx + CPURTX_TX_RESULT);
		writel(status & ~BIT(0), priv->cpurtx + CPURTX_TX_STATUS);

		slot = sd5113_eth_tx_find(priv, hw_addr);
		if (atomic_cmpxchg(&priv->dma_tx_done_logged, 0, 1) == 0)
			dev_info(priv->dev,
				 "first TX completion: device_addr=0x%08x matched=%u\n",
				 hw_addr, slot >= 0);

		if (slot >= 0) {
			done[done_count++] = priv->tx_buf[slot];
			priv->tx_buf[slot].skb->dev->stats.tx_packets++;
			priv->tx_buf[slot].skb->dev->stats.tx_bytes += priv->tx_buf[slot].skb->len;
			priv->tx_buf[slot].skb = NULL;
			priv->tx_buf[slot].dma_addr = 0;
			priv->tx_count--;
			if (done_count == done_max)
				break;
		} else {
			/* UNRESOLVED: a completion address not present in our table.
			 * Do not dereference hardware-provided addresses. The vendor
			 * would recover the skb pointer via (addr + 0x2ffffffc), but
			 * mainline intentionally keeps no such raw CPU alias. */
			priv->ndev->stats.tx_errors++;
		}
	}

	return done_count;
}

/* CONFIRMED: vendor eth_reclaim_tx_buffer() reports a completion when
 * CPURTX_TX_STATUS bit0 is set, returns the device-domain buffer address
 * from CPURTX_TX_RESULT, then clears bit0 in CPURTX_TX_STATUS. CONFIRMED:
 * vendor drains all available completions before continuing TX submission. */
static bool sd5113_eth_tx_ready(struct sd5113_eth *priv)
{
	return priv->tx_count < SD5113_TX_MAX_OUTSTANDING &&
	       !(readl(priv->cpurtx + CPURTX_TX_KICK) & CPURTX_TX_PENDING);
}

static void sd5113_eth_tx_reclaim(struct sd5113_eth *priv)
{
	struct sd5113_tx_buf done[SD5113_TX_MAX_OUTSTANDING];
	unsigned long flags;
	unsigned int i, done_count;

	spin_lock_irqsave(&priv->lock, flags);
	done_count = sd5113_eth_tx_reclaim_locked(priv, done,
						      ARRAY_SIZE(done));
	if (priv->running && netif_queue_stopped(priv->ndev) &&
	    sd5113_eth_tx_ready(priv))
		netif_wake_queue(priv->ndev);
	if (priv->running && priv->optic && netif_running(priv->optic) &&
	    netif_queue_stopped(priv->optic) && sd5113_eth_tx_ready(priv))
		netif_wake_queue(priv->optic);
	spin_unlock_irqrestore(&priv->lock, flags);

	for (i = 0; i < done_count; i++) {
		struct sk_buff *skb = done[i].skb;

		dma_unmap_single(priv->dev, done[i].dma_addr,
				 skb->len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);
	}
}

static void sd5113_eth_mask_irq(struct sd5113_eth *priv, bool mask)
{
	u32 value;

	if (priv->irq_regs) {
		regmap_update_bits(priv->irq_regs, GLB_IRQ_MASK, GLB_ETH_IRQ_BIT,
				   mask ? GLB_ETH_IRQ_BIT : 0);
		return;
	}
	value = readl(priv->glb + GLB_IRQ_MASK);

	if (mask)
		value |= GLB_ETH_IRQ_BIT;
	else
		value &= ~GLB_ETH_IRQ_BIT;
	writel(value, priv->glb + GLB_IRQ_MASK);
}

static void sd5113_eth_schedule_poll(struct sd5113_eth *priv)
{
	if (priv->running && napi_schedule_prep(&priv->napi)) {
		sd5113_eth_mask_irq(priv, true);
		__napi_schedule(&priv->napi);
	}
}

static void sd5113_eth_service(struct work_struct *work)
{
	struct sd5113_eth *priv = container_of(to_delayed_work(work),
					      struct sd5113_eth, service_work);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	sd5113_eth_schedule_poll(priv);
	spin_unlock_irqrestore(&priv->lock, flags);
}

static void sd5113_eth_schedule_service(struct sd5113_eth *priv)
{
	if (priv->running)
		schedule_delayed_work(&priv->service_work,
				      msecs_to_jiffies(SD5113_SERVICE_MS));
}

static void sd5113_eth_program_tx(struct sd5113_eth *priv, dma_addr_t addr,
				   unsigned int len, u32 route)
{
	if (route) {
		writel((len << 4) | 1, priv->cpurtx + CPURTX_TX_CTRL0);
		writel(((route & 0xfff) << 10) | (((route >> 12) & 7) << 6) | BIT(5),
		       priv->cpurtx + CPURTX_TX_CTRL1);
	} else if (priv->fe) {
		/* Match U-Boot's LAN send path at 0x9050d9a4/0x9050d440. */
		writel(len << 4, priv->cpurtx + CPURTX_TX_CTRL0);
		writel(SD5113_FE_TX_CTRL1, priv->cpurtx + CPURTX_TX_CTRL1);
	} else {
		writel((len << 4) | (len << 21),
		       priv->cpurtx + CPURTX_TX_CTRL0);
	}

	writel((u32)addr, priv->cpurtx + CPURTX_TX_BUFADDR);
	writel(readl(priv->cpurtx + CPURTX_TX_KICK) | CPURTX_TX_PENDING,
	       priv->cpurtx + CPURTX_TX_KICK);
}

static bool sd5113_bridge_allows(const u8 filters[2][26], const u8 *data, size_t len)
{
	u16 proto = get_unaligned_be16(data + 12);
	bool tagged = proto == ETH_P_8021Q || proto == ETH_P_8021AD;
	int port;

	if (tagged && len < VLAN_ETH_HLEN)
		return false;
	for (port = 0; port < 2; port++) {
		const u8 *filter = filters[port];
		unsigned int op = filter[0], i;
		u16 mask, tci;

		if (!op)
			continue;
		if (!tagged) {
			if (!(op & 1))
				return false;
			continue;
		}
		if (op == 1)
			return false;
		if (op == 2)
			continue;
		mask = op <= 16 ? VLAN_VID_MASK : op <= 18 ? 0xe000 : 0xffff;
		tci = get_unaligned_be16(data + 14) & mask;
		for (i = 0; i < filter[1]; i++)
			if ((get_unaligned_be16(filter + 2 + i * 2) & mask) == tci)
				break;
		if (i == filter[1])
			return false;
	}
	return true;
}

/* The mapper sees the frame after any VEIP VLAN treatment. */
static unsigned int sd5113_tx_pbit(struct sk_buff *skb, struct sd5113_eth *priv)
{
	u8 header[VLAN_ETH_HLEN];
	const u8 *h;
	u16 proto;
	unsigned int dscp = 0;

	if ((priv->gem_route >> 19) & VLAN_VID_MASK)
		return priv->qos.tag_pcp;
	if (skb_vlan_tag_present(skb))
		return skb_vlan_tag_get_prio(skb);
	h = skb_header_pointer(skb, 0, sizeof(header), header);
	if (h) {
		proto = get_unaligned_be16(h + 12);
		if (proto == ETH_P_8021Q || proto == ETH_P_8021AD)
			return h[14] >> 5;
		if (proto == ETH_P_IP)
			dscp = h[15] >> 2;
		else if (proto == ETH_P_IPV6)
			dscp = ((h[14] & 15) << 2) | (h[15] >> 6);
	}
	return priv->qos.unmarked == 8 ? priv->qos.dscp[dscp] : priv->qos.unmarked;
}

static netdev_tx_t sd5113_xmit(struct sk_buff *skb, struct net_device *ndev,
			       struct sd5113_eth *priv)
{
	u32 route = 0;
	unsigned int min_len = ETH_ZLEN;
	dma_addr_t dma_addr;
	unsigned long flags;
	unsigned int slot;

	/* CONFIRMED from vendor eth_transmit(): CPURTX_TX_STATUS bit0 means a
	 * completion is pending, not a conventional TX-busy bit. Retire all
	 * completions before applying the vendor's nine-buffer limit. */
	sd5113_eth_tx_reclaim(priv);

	if (skb->len > SD5113_TX_FRAME_MAX) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&priv->lock, flags);
	if (!priv->running)
		goto free_skb;
	if (ndev == priv->optic) {
		route = priv->qos.route[sd5113_tx_pbit(skb, priv)];
		if (!route)
			goto free_skb;
	}
	if (!sd5113_eth_tx_ready(priv))
		goto stop_queue;

	for (slot = 0; slot < SD5113_TX_MAX_OUTSTANDING; slot++)
		if (!priv->tx_buf[slot].skb)
			break;

	if (WARN_ON_ONCE(slot == SD5113_TX_MAX_OUTSTANDING))
		goto stop_queue;
	/* Serialize the two producers through descriptor publication. */
	if ((route >> 19) & VLAN_VID_MASK) {
		if (skb->len > ETH_FRAME_LEN || skb_vlan_tagged(skb))
			goto free_skb;
		if (__vlan_insert_tag(skb, htons(ETH_P_8021Q), (priv->qos.tag_pcp << VLAN_PRIO_SHIFT) |
				     ((route >> 19) & VLAN_VID_MASK)))
			goto free_skb;
	}
	if (skb_linearize(skb))
		goto free_skb;
	if (route && (skb->len < ETH_HLEN ||
		      !sd5113_bridge_allows(priv->qos.filters, skb->data, skb->len)))
		goto free_skb;
	/* The switch removes the transport tag at the copper port. Leave a
	 * full minimum Ethernet frame after that four-byte removal.
	 */
	if (ndev == priv->ndev && priv->fe && priv->fe->broadcast &&
	    skb->len >= VLAN_ETH_HLEN &&
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) == ETH_P_8021Q)
		min_len += VLAN_HLEN;
	if (skb_put_padto(skb, min_len))
		goto count_drop;

	dma_addr = dma_map_single(priv->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, dma_addr))
		goto free_skb;

	if (atomic_cmpxchg(&priv->dma_tx_map_logged, 0, 1) == 0)
		dev_dbg(priv->dev, "first TX DMA mapping: dma=%pad len=%u\n",
			&dma_addr, skb->len);

	skb->dev = ndev;
	priv->tx_buf[slot].skb = skb;
	priv->tx_buf[slot].dma_addr = dma_addr;
	priv->tx_count++;

	sd5113_eth_program_tx(priv, dma_addr, skb->len, route);

	if (priv->tx_count >= SD5113_TX_MAX_OUTSTANDING)
		netif_stop_queue(ndev);
	sd5113_eth_schedule_service(priv);
	spin_unlock_irqrestore(&priv->lock, flags);

	return NETDEV_TX_OK;

free_skb:
	dev_kfree_skb_any(skb);
count_drop:
	spin_unlock_irqrestore(&priv->lock, flags);
	ndev->stats.tx_dropped++;
	return NETDEV_TX_OK;

stop_queue:
	netif_stop_queue(ndev);
	sd5113_eth_schedule_service(priv);
	spin_unlock_irqrestore(&priv->lock, flags);
	return NETDEV_TX_BUSY;
}

/* ---------------------------------------------------------------------
 * RX
 * ------------------------------------------------------------------- */

static void sd5113_eth_ack_rx_slot(struct sd5113_eth *priv, unsigned int bit)
{
	/* CONFIRMED (confirmed-map.json): write bitwise inverse of the
	 * handled bit - NOT write-1-to-clear. */
	writel(~BIT(bit), priv->cpurtx + CPURTX_CAUSE);
}

static void sd5113_eth_pull_cpu_tag(struct sk_buff *skb)
{
	u8 *data = skb->data;

	if (skb->len < VLAN_ETH_HLEN ||
	    get_unaligned_be16(data + 2 * ETH_ALEN) != ETH_P_8021Q ||
	    (get_unaligned_be16(data + 2 * ETH_ALEN + 2) & VLAN_VID_MASK) !=
	    SD5113_FE_CPU_VID)
		return;

	/* Remove only the observed outer CPU-path VLAN; keep any inner tag. */
	memmove(data + VLAN_HLEN, data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
}

static void sd5113_eth_rx_slot(struct sd5113_eth *priv, unsigned int bit)
{
	struct net_device *ndev = priv->ndev;
	u32 ctrl, word1;
	unsigned int len;
	int idx;
	struct sk_buff *skb;

	ctrl = readl(priv->cpurtx + SD5113_RX_SLOT_CTRL(bit));
	word1 = readl(priv->cpurtx + SD5113_RX_SLOT_WORD1(bit));
	len = RX_CTRL_LEN(ctrl);

	idx = sd5113_eth_find_rx_buf(priv, word1);
	if (atomic_cmpxchg(&priv->dma_rx_done_logged, 0, 1) == 0)
		dev_info(priv->dev, "first RX completion: ctrl=%#x len=%u matched=%u\n",
			 ctrl, len, idx >= 0);
	if (idx < 0) {
		ndev->stats.rx_errors++;
		goto ack;
	}

	skb = priv->rx_pool[idx].skb;
	dma_unmap_single(priv->dev, priv->rx_pool[idx].dma_addr,
			  SD5113_RX_BUF_SIZE, DMA_FROM_DEVICE);
	priv->rx_pool[idx].skb = NULL;

	if (len < ETH_HLEN || len > SD5113_RX_BUF_SIZE) {
		ndev->stats.rx_errors++;
		dev_kfree_skb_any(skb);
		goto ack;
	}

	skb_put(skb, len);
	if (RX_CTRL_CATEGORY(ctrl) == 5) {
		unsigned long flags;
		u32 route;
		u16 vid;
		bool allowed;

		spin_lock_irqsave(&priv->lock, flags);
		route = priv->gem_route;
		allowed = sd5113_bridge_allows(priv->qos.filters, skb->data, len);
		spin_unlock_irqrestore(&priv->lock, flags);
		vid = (route >> 19) & VLAN_VID_MASK;

		ndev = priv->optic;
		if (!ndev || !netif_running(ndev) || !route || !allowed ||
		    (vid && (len < VLAN_ETH_HLEN ||
		     get_unaligned_be16(skb->data + 12) != ETH_P_8021Q ||
		     (get_unaligned_be16(skb->data + 14) & VLAN_VID_MASK) != vid))) {
			if (ndev)
				ndev->stats.rx_dropped++;
			dev_kfree_skb_any(skb);
			goto ack;
		}
		if (vid) {
			skb->priority = get_unaligned_be16(skb->data + 14) >> VLAN_PRIO_SHIFT;
			memmove(skb->data + VLAN_HLEN, skb->data, ETH_ALEN * 2);
			skb_pull(skb, VLAN_HLEN);
		}
	} else if (priv->fe) {
		sd5113_eth_pull_cpu_tag(skb);
	}
	len = skb->len;
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	netif_receive_skb(skb);

ack:
	sd5113_eth_ack_rx_slot(priv, bit);
}

static bool sd5113_eth_finish_rx_irq(struct sd5113_eth *priv)
{
	if (readl(priv->cpurtx + CPURTX_CAUSE) & SD5113_RX_SLOT_MASK)
		return false;

	/* With no RX slots left, clear the separate RX latch as hi_eth does. */
	if (readl(priv->glb + GLB_IRQ_CAUSE) & GLB_ETH_IRQ_BIT)
		writel(GLB_ETH_IRQ_BIT, priv->glb + GLB_IRQ_CAUSE);

	/* A frame could have arrived between the empty check and the clear. */
	return !(readl(priv->cpurtx + CPURTX_CAUSE) & SD5113_RX_SLOT_MASK);
}

static int sd5113_eth_poll(struct napi_struct *napi, int budget)
{
	struct sd5113_eth *priv = container_of(napi, struct sd5113_eth, napi);
	unsigned long flags;
	int work_done = 0;
	bool refill_failed, complete;
	u32 cause;
	unsigned int bit;

	sd5113_eth_tx_reclaim(priv);
	if (!budget)
		return 0;

	cause = readl(priv->cpurtx + CPURTX_CAUSE) & SD5113_RX_SLOT_MASK;

	for (bit = 0; bit < SD5113_RX_NUM_SLOTS && work_done < budget; bit++) {
		if (!(cause & BIT(bit)))
			continue;
		sd5113_eth_rx_slot(priv, bit);
		work_done++;
	}

	refill_failed = sd5113_eth_fill_rx_pool(priv) != 0;
	if (work_done < budget && !sd5113_eth_finish_rx_irq(priv))
		work_done = budget;

	/* Keep teardown and rescheduling serialized with IRQ rearming. */
	spin_lock_irqsave(&priv->lock, flags);
	complete = work_done < budget && napi_complete_done(napi, work_done);
	if (priv->running) {
		if (complete)
			sd5113_eth_mask_irq(priv, false);
		if (refill_failed || priv->tx_count || netif_queue_stopped(priv->ndev))
			sd5113_eth_schedule_service(priv);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return work_done;
}

/* ---------------------------------------------------------------------
 * IRQ
 * ------------------------------------------------------------------- */

static irqreturn_t sd5113_eth_irq(int irq, void *data)
{
	struct sd5113_eth *priv = data;
	unsigned long flags;
	u32 cause, mask;
	irqreturn_t handled = IRQ_NONE;

	spin_lock_irqsave(&priv->lock, flags);
	cause = readl(priv->glb + GLB_IRQ_CAUSE);
	mask = readl(priv->glb + GLB_IRQ_MASK);
	if (priv->running && (cause & ~mask & GLB_ETH_IRQ_BIT)) {
		sd5113_eth_schedule_poll(priv);
		handled = IRQ_HANDLED;
	}
	spin_unlock_irqrestore(&priv->lock, flags);
	return handled;
}

/* ---------------------------------------------------------------------
 * net_device_ops
 * ------------------------------------------------------------------- */

static int sd5113_eth_reset_fifo(struct sd5113_eth *priv)
{
	u32 val;
	int ret;

	writel(readl(priv->cpurtx + CPURTX_FIFO_RESET) | BIT(0),
	       priv->cpurtx + CPURTX_FIFO_RESET);
	ret = readl_poll_timeout(priv->cpurtx + CPURTX_FIFO_RESET, val,
				  !(val & BIT(0)), 10, SD5113_POLL_TIMEOUT_US);
	if (ret)
		netdev_err(priv->ndev, "FIFO reset timed out\n");
	else {
		writel(0, priv->cpurtx + CPURTX_TX_STATUS);
		writel(0, priv->cpurtx + CPURTX_CAUSE);
	}

	return ret;
}

static void sd5113_eth_release_buffers(struct sd5113_eth *priv)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(priv->tx_buf); i++) {
		struct sd5113_tx_buf *buf = &priv->tx_buf[i];

		if (!buf->skb)
			continue;
		dma_unmap_single(priv->dev, buf->dma_addr, buf->skb->len,
				 DMA_TO_DEVICE);
		dev_kfree_skb_any(buf->skb);
		buf->skb = NULL;
	}
	priv->tx_count = 0;
	sd5113_eth_drain_rx_pool(priv);
}

/* U-Boot 0x90513b2c reads the straps without reconfiguring any GPIO pin. */
static unsigned int sd5113_fe_strap(struct sd5113_fe *fe, unsigned int pin)
{
	void __iomem *bank = fe->gpio + (pin / 8) * SZ_4K;
	u32 bit = BIT(pin % 8);
	u32 offset;

	if (readl(bank + 0x08) & bit)
		return 0xff;

	offset = (readl(bank + 0x04) & bit) ? 0x00 : 0x50;
	return !!(readl(bank + offset) & bit);
}

static void sd5113_fe_route_group(struct sd5113_fe *fe, unsigned int classifier,
				 unsigned int action, unsigned int port,
				 bool per_priority, u32 dest)
{
	unsigned int prio;

	for (prio = 0; prio < 8; prio++) {
		unsigned int slot = action + (per_priority ? prio : prio / 2);

		writel(0x04000000 | (prio << 9) | port,
		       fe->policy + slot * sizeof(u32));
		writel(dest, fe->dest + slot * sizeof(u32));
		writel(0x2000 | (prio << 14) | slot,
		       fe->classifier + (classifier + prio) * sizeof(u32));
	}
}

/* Replay the board-specific LAN table from U-Boot 0x9050e22c. Its bootm
 * clears the classifier (0x90506b30 -> 0x9050d868) before entering Linux.
 */
static void sd5113_fe_program_routes(struct sd5113_fe *fe, unsigned int type,
				    unsigned int aux)
{
	bool extra = type == 4 || (type == 8 && aux == 4) || type == 5;

	if (extra || type == 21 || type == 22 || type == 23) {
		sd5113_fe_route_group(fe, 0x240, 0x24, 9, false, 0x00012010);
		if (extra || type == 23)
			sd5113_fe_route_group(fe, 0x280, 0x28, 10, false, 0x00012010);
		if (extra) {
			sd5113_fe_route_group(fe, 0x2c0, 0x2c, 11, false, 0x00012010);
			sd5113_fe_route_group(fe, 0x300, 0x30, 12, false, 0x00012010);
		}
	} else {
		sd5113_fe_route_group(fe, 0x200, 0x20, 8, false, 0x00012010);
		if (type == 19 || type == 20)
			sd5113_fe_route_group(fe, 0x240, 0x24, 9, false, 0x00012010);
	}
	sd5113_fe_route_group(fe, 0x340, 0x34, 13, true, 0x00001e10);
}

static void sd5113_lan_vlans_stop(struct sd5113_fe *fe)
{
	unsigned int i, vid;

	if (!fe || !fe->bc_tail)
		return;

	for (i = 0; i < SD5113_LAN_PORT_COUNT; i++) {
		vid = SD5113_LAN_VID_BASE + i;
		writel(fe->saved_lan[i].head, fe->broadcast + vid);
		writel(fe->saved_lan[i].members, fe->members + vid);
	}
	readl(fe->broadcast + SD5113_LAN_VID_BASE);
	if (fe->bc_tail_owned) {
		writel(fe->saved_tail, fe->broadcast + fe->bc_tail);
		readl(fe->broadcast + fe->bc_tail);
	}
	fe->bc_tail_owned = false;
	fe->bc_tail = 0;
}

static int sd5113_lan_vlans_start(struct sd5113_fe *fe)
{
	DECLARE_BITMAP(used, MCD_TAIL_COUNT);
	unsigned int i, vid, next, tail, first_port = FE_CPU_PORT;
	u32 entry, members, head, trunk, terminal;

	if (!fe || !fe->broadcast)
		return 0;
	trunk = FIELD_PREP(MCD_PORT, fe->trunk_port);

	/* U-Boot's native VLAN chain terminates at CPU port 6. A terminal
	 * can be shared without modifying it. Its next pointer is ignored,
	 * but the forwarding fields including bits 23:16 must match.
	 */
	for (i = MCD_TAIL_BASE; i < MCD_WORDS; i++) {
		terminal = readl(fe->broadcast + i) & (MCD_FIELDS & ~MCD_NEXT);
		if (terminal == (MCD_VALID | trunk))
			break;
		if (terminal == (MCD_VALID | FIELD_PREP(MCD_PORT, FE_CPU_PORT))) {
			first_port = fe->trunk_port;
			break;
		}
	}
	if (i < MCD_WORDS) {
		fe->bc_tail = i;
	} else {
		/* Invalid records can retain payload. Exclude live links to
		 * them too, so publishing one cannot extend another chain.
		 */
		bitmap_zero(used, MCD_TAIL_COUNT);
		for (i = 0; i < MCD_WORDS; i++) {
			entry = readl(fe->broadcast + i);
			if (!(entry & MCD_VALID))
				continue;
			if (i >= MCD_TAIL_BASE)
				__set_bit(i - MCD_TAIL_BASE, used);
			if (!(entry & MCD_HAS_NEXT))
				continue;
			next = FIELD_GET(MCD_NEXT, entry);
			if (next >= MCD_TAIL_BASE && next < MCD_WORDS)
				__set_bit(next - MCD_TAIL_BASE, used);
		}
		tail = find_first_zero_bit(used, MCD_TAIL_COUNT);
		if (tail == MCD_TAIL_COUNT)
			return -ENOSPC;
		fe->bc_tail = MCD_TAIL_BASE + tail;
		fe->bc_tail_owned = true;
		fe->saved_tail = readl(fe->broadcast + fe->bc_tail);
	}
	head = MCD_VALID | MCD_HAS_NEXT | FIELD_PREP(MCD_PORT, first_port) | fe->bc_tail;
	members = BIT(FE_CPU_PORT) | BIT(fe->trunk_port);
	for (i = 0; i < SD5113_LAN_PORT_COUNT; i++) {
		vid = SD5113_LAN_VID_BASE + i;
		fe->saved_lan[i].head = readl(fe->broadcast + vid);
		fe->saved_lan[i].members = readl(fe->members + vid);
	}
	if (fe->bc_tail_owned) {
		entry = MCD_VALID | trunk;
		writel((fe->saved_tail & ~MCD_FIELDS) | entry, fe->broadcast + fe->bc_tail);
		if ((readl(fe->broadcast + fe->bc_tail) & MCD_FIELDS) != entry)
			goto failed;
	}
	for (i = 0; i < SD5113_LAN_PORT_COUNT; i++) {
		vid = SD5113_LAN_VID_BASE + i;
		writel((fe->saved_lan[i].head & ~MCD_FIELDS) | head, fe->broadcast + vid);
		writel((fe->saved_lan[i].members & ~CAP_MEMBER_MASK) | members,
		       fe->members + vid);
		if ((readl(fe->broadcast + vid) & MCD_FIELDS) != head ||
		    (readl(fe->members + vid) & CAP_MEMBER_MASK) != members)
			goto failed;
	}
	return 0;

failed:
	sd5113_lan_vlans_stop(fe);
	return -EIO;
}

static void sd5113_fe_start(struct sd5113_eth *priv)
{
	struct sd5113_fe *fe = priv->fe;
	unsigned int type, aux, strap;

	if (!fe)
		return;

	/* Same strap packing as U-Boot 0x90513b8c/0x90513c40. */
	type = sd5113_fe_strap(fe, 14);
	type |= sd5113_fe_strap(fe, 15) << 1;
	type |= sd5113_fe_strap(fe, 30) << 2;
	strap = sd5113_fe_strap(fe, 31);
	type |= strap << 3;
	if (!(strap & 1))
		type |= sd5113_fe_strap(fe, 26) << 4;
	aux = sd5113_fe_strap(fe, 16);
	aux |= sd5113_fe_strap(fe, 17) << 1;
	aux |= sd5113_fe_strap(fe, 27) << 2;

	sd5113_fe_program_routes(fe, type, aux);

	/* CPU-path controls written by U-Boot 0x9050e55c during LAN bring-up. */
	fe->saved_cpu = readl(fe->cpu + FE_CPU_CTRL);
	fe->saved_filter = readl(fe->cpu + FE_CPU_FILTER);
	fe->saved_mode = readl(fe->members + SD5113_FE_CPU_VID);
	fe->saved_tpid = readl(priv->glb + GLB_CPU_VLAN_TPID);
	fe->saved_port_cfg = readl(fe->port_cfg);
	writel(fe->saved_filter & ~BIT(0), fe->cpu + FE_CPU_FILTER);
	writel(ETH_P_8021Q, priv->glb + GLB_CPU_VLAN_TPID);
	writel(0x327, fe->port_cfg);
	writel(0x7f, fe->members + SD5113_FE_CPU_VID);
	writel(fe->saved_cpu | BIT(1), fe->cpu + FE_CPU_CTRL);
	readl(fe->cpu + FE_CPU_CTRL);

	netdev_info(priv->ndev, "U-Boot LAN classifier restored (straps %#x/%#x)\n",
		    type, aux);
}

static void sd5113_fe_stop(struct sd5113_eth *priv)
{
	struct sd5113_fe *fe = priv->fe;

	if (!fe)
		return;

	writel(fe->saved_cpu, fe->cpu + FE_CPU_CTRL);
	writel(fe->saved_filter, fe->cpu + FE_CPU_FILTER);
	writel(fe->saved_mode, fe->members + SD5113_FE_CPU_VID);
	writel(fe->saved_tpid, priv->glb + GLB_CPU_VLAN_TPID);
	writel(fe->saved_port_cfg, fe->port_cfg);
	readl(fe->cpu + FE_CPU_CTRL);
}

static int sd5113_eth_open(struct net_device *ndev)
{
	struct sd5113_eth *priv = netdev_priv(ndev);
	unsigned long flags;
	int ret;

	netif_carrier_off(ndev);
	ret = sd5113_eth_reset_fifo(priv);
	if (ret)
		return ret;
	sd5113_eth_release_buffers(priv);

	ret = request_irq(priv->irq, sd5113_eth_irq, IRQF_SHARED, DRV_NAME, priv);
	if (ret)
		return ret;

	ret = sd5113_eth_fill_rx_pool(priv);
	if (!ret) {
		ret = sd5113_lan_vlans_start(priv->fe);
		if (ret) {
			netdev_err(ndev, "LAN VLAN forwarding setup failed: %d (VID1=%08x tail0=%08x tail5=%08x)\n",
				   ret, readl(priv->fe->broadcast + 1),
				   readl(priv->fe->broadcast + MCD_TAIL_BASE),
				   readl(priv->fe->broadcast + MCD_TAIL_BASE + 5));
		} else if (priv->fe && priv->fe->bc_tail) {
			netdev_info(ndev, "LAN VLANs %u-%u ready (%s tail %#x)\n",
				    SD5113_LAN_VID_BASE, SD5113_LAN_VID_BASE + SD5113_LAN_PORT_COUNT - 1,
				    priv->fe->bc_tail_owned ? "allocated" : "shared", priv->fe->bc_tail);
		}
	}
	if (ret) {
		free_irq(priv->irq, priv);
		if (!sd5113_eth_reset_fifo(priv))
			sd5113_eth_release_buffers(priv);
		return ret;
	}

	sd5113_fe_start(priv);
	napi_enable(&priv->napi);
	spin_lock_irqsave(&priv->lock, flags);
	priv->running = true;
	if (priv->optic && priv->gem_route)
		netif_carrier_on(priv->optic);
	sd5113_eth_mask_irq(priv, false);
	spin_unlock_irqrestore(&priv->lock, flags);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int sd5113_eth_stop(struct net_device *ndev)
{
	struct sd5113_eth *priv = netdev_priv(ndev);
	unsigned long flags;
	int ret;

	netif_carrier_off(ndev);
	spin_lock_irqsave(&priv->lock, flags);
	priv->running = false;
	if (priv->optic)
		netif_carrier_off(priv->optic);
	sd5113_eth_mask_irq(priv, true);
	spin_unlock_irqrestore(&priv->lock, flags);
	netif_tx_disable(ndev);
	free_irq(priv->irq, priv);
	cancel_delayed_work_sync(&priv->service_work);
	napi_disable(&priv->napi);

	/* napi_disable waits for ownership, not the last instructions of poll(). */
	spin_lock_irqsave(&priv->lock, flags);
	spin_unlock_irqrestore(&priv->lock, flags);
	sd5113_eth_tx_reclaim(priv);
	sd5113_fe_stop(priv);
	sd5113_lan_vlans_stop(priv->fe);
	ret = sd5113_eth_reset_fifo(priv);
	if (!ret)
		sd5113_eth_release_buffers(priv);
	return 0;
}

static netdev_tx_t sd5113_eth_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	return sd5113_xmit(skb, dev, netdev_priv(dev));
}

static netdev_tx_t sd5113_optic_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sd5113_eth **priv = netdev_priv(dev);

	return sd5113_xmit(skb, dev, *priv);
}

static int sd5113_optic_open(struct net_device *dev)
{
	struct sd5113_eth **parent = netdev_priv(dev);
	int ret = dev_open((*parent)->ndev, NULL);

	if (!ret)
		netif_start_queue(dev);
	return ret;
}

static int sd5113_optic_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static const struct net_device_ops sd5113_optic_ops = {
	.ndo_open = sd5113_optic_open,
	.ndo_stop = sd5113_optic_stop,
	.ndo_start_xmit = sd5113_optic_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* The bootloader uses classifier groups 8..13 for LAN and CPU.
 * Route the remaining ingress groups to the CPU only while a data GEM is
 * admitted. Restore the original rules on teardown.
 */
static void sd5113_pon_classifiers(struct sd5113_eth *priv, bool enable)
{
	struct sd5113_fe *fe = priv->fe;
	unsigned int group, prio, slot;

	if (!fe)
		return;
	for (slot = 0; slot < 32; slot++) {
		if (enable) {
			priv->pon_policy[slot] = readl(fe->policy + slot * 4);
			priv->pon_dest[slot] = readl(fe->dest + slot * 4);
		} else {
			writel(priv->pon_policy[slot], fe->policy + slot * 4);
			writel(priv->pon_dest[slot], fe->dest + slot * 4);
		}
	}
	for (group = 0; group < 8; group++) {
		for (prio = 0; prio < 8; prio++) {
			void __iomem *reg = fe->classifier + (group * 0x40 + prio) * 4;

			if (enable)
				priv->pon_classifier[group][prio] = readl(reg);
			else
				writel(priv->pon_classifier[group][prio], reg);
		}
		if (enable)
			sd5113_fe_route_group(fe, group * 0x40, group * 4, group, false, 0x00012010);
	}
	readl(fe->classifier);
}

int sd5113_eth_set_gem(const struct sd5113_gem_profile *profile)
{
	struct sd5113_eth *priv;
	unsigned long flags;
	int ret = -ENODEV;

	u32 primary = 0;
	int p;

	if (profile)
		for (p = 0; p < ARRAY_SIZE(profile->route) && !primary; p++)
			primary = profile->route[p];
	mutex_lock(&sd5113_port_lock);
	priv = sd5113_pon_mac;
	if (priv) {
		spin_lock_irqsave(&priv->lock, flags);
		if (!!priv->gem_route != !!primary)
			sd5113_pon_classifiers(priv, !!primary);
		if (profile)
			priv->qos = *profile;
		else
			memset(&priv->qos, 0, sizeof(priv->qos));
		WRITE_ONCE(priv->gem_route, primary);
		if (primary && priv->running)
			netif_carrier_on(priv->optic);
		else
			netif_carrier_off(priv->optic);
		spin_unlock_irqrestore(&priv->lock, flags);
		ret = 0;
	}
	mutex_unlock(&sd5113_port_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sd5113_eth_set_gem);

static const struct net_device_ops sd5113_eth_netdev_ops = {
	.ndo_open		= sd5113_eth_open,
	.ndo_stop		= sd5113_eth_stop,
	.ndo_start_xmit		= sd5113_eth_start_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ---------------------------------------------------------------------
 * probe / remove
 * ------------------------------------------------------------------- */

static int sd5113_fe_probe(struct platform_device *pdev, struct sd5113_eth *priv)
{
	struct device *dev = &pdev->dev;
	struct device_node *gpio_node;
	struct resource gpio;
	struct sd5113_fe *fe;
	int ret;

	if (!of_property_read_bool(dev->of_node, "hisilicon,restore-uboot-lan"))
		return 0;

	fe = devm_kzalloc(dev, sizeof(*fe), GFP_KERNEL);
	if (!fe)
		return -ENOMEM;

	/* Older DTs place port_cfg in the large GLB window. The combined
	 * board leaves the intervening GPON registers to their own driver.
	 */
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "cpu-port-cfg")) {
		fe->port_cfg = devm_platform_ioremap_resource_byname(pdev, "cpu-port-cfg");
		if (IS_ERR(fe->port_cfg))
			return PTR_ERR(fe->port_cfg);
	} else {
		struct resource *glb = platform_get_resource(pdev, IORESOURCE_MEM, 0);

		if (!glb || resource_size(glb) < GLB_CPU_PORT_CFG + sizeof(u32))
			return -EINVAL;
		fe->port_cfg = priv->glb + GLB_CPU_PORT_CFG;
	}

	fe->cpu = devm_platform_ioremap_resource_byname(pdev, "fe-cpu");
	if (IS_ERR(fe->cpu))
		return PTR_ERR(fe->cpu);
	fe->members = devm_platform_ioremap_resource_byname(pdev, "fe-mode");
	if (IS_ERR(fe->members))
		return PTR_ERR(fe->members);
	if (of_property_present(dev->of_node, "hisilicon,lan-trunk-port")) {
		struct resource *cap, *bc;

		ret = of_property_read_u32(dev->of_node, "hisilicon,lan-trunk-port",
					   &fe->trunk_port);
		if (ret || fe->trunk_port > 4)
			return -EINVAL;
		cap = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fe-mode");
		bc = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fe-broadcast");
		if (!cap || resource_size(cap) < (SD5113_LAN_VID_BASE + SD5113_LAN_PORT_COUNT) * 4 ||
		    !bc || resource_size(bc) < MCD_WORDS * 4)
			return -EINVAL;
		fe->broadcast = devm_ioremap_resource(dev, bc);
		if (IS_ERR(fe->broadcast))
			return PTR_ERR(fe->broadcast);
	}
	fe->classifier = devm_platform_ioremap_resource_byname(pdev, "fe-classifier");
	if (IS_ERR(fe->classifier))
		return PTR_ERR(fe->classifier);
	fe->policy = devm_platform_ioremap_resource_byname(pdev, "fe-policy");
	if (IS_ERR(fe->policy))
		return PTR_ERR(fe->policy);
	fe->dest = devm_platform_ioremap_resource_byname(pdev, "fe-dest");
	if (IS_ERR(fe->dest))
		return PTR_ERR(fe->dest);

	gpio_node = of_parse_phandle(dev->of_node, "hisilicon,board-gpio", 0);
	if (!gpio_node)
		return -EINVAL;
	ret = of_address_to_resource(gpio_node, 0, &gpio);
	of_node_put(gpio_node);
	if (ret)
		return ret;
	if (resource_size(&gpio) < 4 * SZ_4K)
		return -EINVAL;

	/* Shared GPIO block: board strap reads only, no pin ownership or writes. */
	fe->gpio = devm_ioremap(dev, gpio.start, 4 * SZ_4K);
	if (!fe->gpio)
		return -ENOMEM;

	priv->fe = fe;
	return 0;
}

static int sd5113_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct sd5113_eth *priv;
	int ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = dev;
	spin_lock_init(&priv->lock);
	INIT_DELAYED_WORK(&priv->service_work, sd5113_eth_service);
	atomic_set(&priv->dma_tx_map_logged, 0);
	atomic_set(&priv->dma_tx_done_logged, 0);
	atomic_set(&priv->dma_rx_map_logged, 0);
	atomic_set(&priv->dma_rx_done_logged, 0);

	/* CPURTx address registers are 32-bit. The vendor's `skb->data -
	 * 0x30000000` is consistent with the ARM linear-map VA->PA delta for
	 * RAM at 0x90000000; let the DMA API produce the device address and
	 * perform cache maintenance. Never apply that subtraction to dma_addr_t. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret,
				     "32-bit DMA is not available\n");

	/* The GLB resource includes the optional LAN CPU-path controls. */
	priv->glb = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->glb))
		return PTR_ERR(priv->glb);

	if (of_device_is_compatible(dev->of_node, "syscon")) {
		priv->irq_regs = syscon_node_to_regmap(dev->of_node);
		if (IS_ERR(priv->irq_regs))
			return PTR_ERR(priv->irq_regs);
	}

	priv->cpurtx = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->cpurtx))
		return PTR_ERR(priv->cpurtx);

	ret = sd5113_fe_probe(pdev, priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot map U-Boot LAN datapath\n");

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	/* HYPOTHESIS: no MAC address source identified in this BSP beyond
	 * userspace jffs2cfg (not part of this archive) - fall back to a
	 * random locally-administered address. */
	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret)
		eth_hw_addr_random(ndev);

	ndev->netdev_ops = &sd5113_eth_netdev_ops;
	ndev->max_mtu = 1500;	/* CPURTX_MTU_CTRL/COUNT exist but their bit
				 * layout was never decoded - relying on
				 * power-on default rather than programming it. */

	netif_napi_add(ndev, &priv->napi, sd5113_eth_poll);

	sd5113_eth_mask_irq(priv, true);

	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&priv->napi);
		return ret;
	}

	if (of_property_read_bool(dev->of_node, "hisilicon,gem-wan")) {
		struct net_device *optic = devm_alloc_etherdev(dev, sizeof(priv));

		ret = -ENOMEM;
		if (!optic)
			goto unregister_lan;
		*(struct sd5113_eth **)netdev_priv(optic) = priv;
		strscpy(optic->name, "optic", IFNAMSIZ);
		SET_NETDEV_DEV(optic, dev);
		eth_hw_addr_inherit(optic, ndev);
		optic->netdev_ops = &sd5113_optic_ops;
		optic->max_mtu = ETH_DATA_LEN;
		netif_carrier_off(optic);
		priv->optic = optic;
		ret = register_netdev(optic);
		if (ret)
			goto unregister_lan;
		mutex_lock(&sd5113_port_lock);
		sd5113_pon_mac = priv;
		mutex_unlock(&sd5113_port_lock);
	}
	platform_set_drvdata(pdev, ndev);
	netdev_info(ndev, "SD5113 CPU-port MAC (round 30, UNTESTED on real hardware, see driver header)\n");
	return 0;

unregister_lan:
	unregister_netdev(ndev);
	netif_napi_del(&priv->napi);
	return ret;
}

static int sd5113_eth_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct sd5113_eth *priv = netdev_priv(ndev);

	if (priv->optic) {
		mutex_lock(&sd5113_port_lock);
		sd5113_pon_mac = NULL;
		mutex_unlock(&sd5113_port_lock);
		unregister_netdev(priv->optic);
	}
	unregister_netdev(ndev);
	netif_napi_del(&priv->napi);

	return 0;
}

static const struct of_device_id sd5113_eth_of_match[] = {
	{ .compatible = "hisilicon,sd5113-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_eth_of_match);

static struct platform_driver sd5113_eth_driver = {
	.probe	= sd5113_eth_probe,
	.remove	= sd5113_eth_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= sd5113_eth_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(sd5113_eth_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 CPU-port Ethernet MAC (reverse-engineered, untested)");
MODULE_LICENSE("GPL");

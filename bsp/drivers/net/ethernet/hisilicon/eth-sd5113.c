// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 "CPU port" Ethernet MAC driver (HG8245 reverse-engineered BSP) */

#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#define DRV_NAME "sd5113-eth"

/* ---------------------------------------------------------------------
 * g_glb_register block - physical 0x42000010, size 8 bytes. CONFIRMED.
 * ------------------------------------------------------------------- */
#define GLB_IRQ_CAUSE		0x00	/* CONFIRMED: bit4 = eth event, read */
#define GLB_IRQ_MASK		0x04	/* CONFIRMED: bit4 = eth event, RMW, mask-on-service */
#define GLB_ETH_IRQ_BIT		BIT(4)

/* ---------------------------------------------------------------------
 * g_cpurtx_register block - physical 0x42100000, size 0xd8 bytes.
 * ------------------------------------------------------------------- */
#define CPURTX_TX_KICK		0x00	/* CONFIRMED: bit0 = kick, write 1 */
#define CPURTX_TX_CTRL0		0x04	/* CONFIRMED (round 30): in the
					 * desc[6]==0 ("no VLAN") path, holds
					 * the 11-bit length duplicated at bit
					 * positions [14:4] and [31:21] - see
					 * NOTES_ETH.md round 30. Medium-high
					 * confidence, not 100%. */
#define CPURTX_TX_BUFADDR	0x08	 
#define CPURTX_TX_CTRL1		0x0c	/* CONFIRMED existence, vendor never
					 * writes it in the desc[6]==0 path.
					 * NOT used by this driver (round 29's
					 * use of this as a second address slot
					 * was wrong - the real address goes to
					 * +0x08, see above). Left unwritten. */
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
#define SD5113_RX_BUF_SIZE	1536
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

struct sd5113_eth {
	struct net_device	*ndev;
	struct device		*dev;
	void __iomem		*glb;		/* 0x42000010, 8 bytes */
	void __iomem		*cpurtx;	/* 0x42100000, 0xd8 bytes */
	int			irq;
	struct napi_struct	napi;
	spinlock_t		lock;

	struct sd5113_tx_buf	tx_buf[SD5113_TX_MAX_OUTSTANDING];
	unsigned int		tx_count;
	atomic_t		dma_tx_map_logged;
	atomic_t		dma_tx_done_logged;
	atomic_t		dma_rx_map_logged;

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

	skb = netdev_alloc_skb_ip_align(priv->ndev, SD5113_RX_BUF_SIZE);
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
static void sd5113_eth_fill_rx_pool(struct sd5113_eth *priv)
{
	unsigned int i;

	for (i = 0; i < SD5113_RX_POOL_SIZE; i++) {
		if (sd5113_eth_fill_rx_slot(priv, i))
			netdev_warn(priv->ndev,
				    "failed to fill RX pool slot %u\n", i);
	}
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
		if ((u32)priv->rx_pool[i].dma_addr == word1)
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

	for (;;) {
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
static void sd5113_eth_tx_reclaim(struct sd5113_eth *priv)
{
	struct sd5113_tx_buf done[SD5113_TX_MAX_OUTSTANDING];
	unsigned long flags;
	unsigned int i, done_count;
	bool wake_queue;

	spin_lock_irqsave(&priv->lock, flags);
	done_count = sd5113_eth_tx_reclaim_locked(priv, done,
						      ARRAY_SIZE(done));
	wake_queue = netif_queue_stopped(priv->ndev) &&
			     priv->tx_count < SD5113_TX_MAX_OUTSTANDING;
	spin_unlock_irqrestore(&priv->lock, flags);

	for (i = 0; i < done_count; i++) {
		struct sk_buff *skb = done[i].skb;

		dma_unmap_single(priv->dev, done[i].dma_addr,
				 skb->len, DMA_TO_DEVICE);
		priv->ndev->stats.tx_packets++;
		priv->ndev->stats.tx_bytes += skb->len;
		dev_kfree_skb_any(skb);
	}

	if (wake_queue)
		netif_wake_queue(priv->ndev);
}

static netdev_tx_t sd5113_eth_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct sd5113_eth *priv = netdev_priv(ndev);
	dma_addr_t dma_addr;
	unsigned long flags;
	unsigned int slot;
	u32 len_field;

	/* CONFIRMED from vendor eth_transmit(): CPURTX_TX_STATUS bit0 means a
	 * completion is pending, not a conventional TX-busy bit. Retire all
	 * completions before applying the vendor's nine-buffer limit. */
	sd5113_eth_tx_reclaim(priv);

	if (skb->len > SD5113_RX_BUF_SIZE) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	dma_addr = dma_map_single(priv->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(priv->dev, dma_addr)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	if (atomic_cmpxchg(&priv->dma_tx_map_logged, 0, 1) == 0)
		dev_info(priv->dev, "first TX DMA mapping: dma=%pad len=%u\n",
			 &dma_addr, skb->len);

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->tx_count >= SD5113_TX_MAX_OUTSTANDING) {
		/* Stop while holding the same lock used by TX reclaim. That prevents
		 * a completion/wakeup from racing between the full check and stop. */
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->lock, flags);
		dma_unmap_single(priv->dev, dma_addr, skb->len, DMA_TO_DEVICE);
		return NETDEV_TX_BUSY;
	}

	for (slot = 0; slot < SD5113_TX_MAX_OUTSTANDING; slot++)
		if (!priv->tx_buf[slot].skb)
			break;

	if (slot == SD5113_TX_MAX_OUTSTANDING) {
		/* Same serialization as the tx_count-full case above. */
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&priv->lock, flags);
		dma_unmap_single(priv->dev, dma_addr, skb->len, DMA_TO_DEVICE);
		return NETDEV_TX_BUSY;
	}

	priv->tx_buf[slot].skb = skb;
	priv->tx_buf[slot].dma_addr = dma_addr;
	priv->tx_count++;

	/* CONFIRMED: buffer address goes to +0x08. Vendor's subtraction is
	 * the linear-map VA->PA conversion for this RAM layout. dma_map_single()
	 * returns the device-visible address, including any platform DMA offset.
	 * The 32-bit mask above makes the register-width contract explicit. */
	writel((u32)dma_addr, priv->cpurtx + CPURTX_TX_BUFADDR);

	/* CONFIRMED-ish (round 30, medium-high confidence): no-VLAN path places
	 * length at both bit fields [14:4] and [31:21]. */
	len_field = (skb->len << 4) | (skb->len << 21);
	writel(len_field, priv->cpurtx + CPURTX_TX_CTRL0);

	/* CONFIRMED: kick = bit0 of TX_KICK, read-modify-write with OR. */
	writel(readl(priv->cpurtx + CPURTX_TX_KICK) | BIT(0),
	       priv->cpurtx + CPURTX_TX_KICK);

	if (priv->tx_count >= SD5113_TX_MAX_OUTSTANDING)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&priv->lock, flags);

	return NETDEV_TX_OK;
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
	if (idx < 0 || !len || len > SD5113_RX_BUF_SIZE) {
		ndev->stats.rx_errors++;
		goto ack;
	}

	skb = priv->rx_pool[idx].skb;
	dma_unmap_single(priv->dev, priv->rx_pool[idx].dma_addr,
			  SD5113_RX_BUF_SIZE, DMA_FROM_DEVICE);
	priv->rx_pool[idx].skb = NULL;

	skb_put(skb, len);
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	netif_receive_skb(skb);

	/* CONFIRMED behaviour (round 30 init-loop evidence): a completed slot
	 * must be refilled with a fresh buffer for hardware to keep using it. */
	if (sd5113_eth_fill_rx_slot(priv, idx))
		netdev_warn(ndev, "failed to refill RX pool slot %d\n", idx);

ack:
	sd5113_eth_ack_rx_slot(priv, bit);
}

static int sd5113_eth_poll(struct napi_struct *napi, int budget)
{
	struct sd5113_eth *priv = container_of(napi, struct sd5113_eth, napi);
	int work_done = 0;
	u32 cause;
	unsigned int bit;

	sd5113_eth_tx_reclaim(priv);

	cause = readl(priv->cpurtx + CPURTX_CAUSE) & GENMASK(SD5113_RX_NUM_SLOTS - 1, 0);

	for (bit = 0; bit < SD5113_RX_NUM_SLOTS && work_done < budget; bit++) {
		if (!(cause & BIT(bit)))
			continue;
		sd5113_eth_rx_slot(priv, bit);
		work_done++;
	}

	if (work_done < budget) {
		napi_complete_done(napi, work_done);
		writel(readl(priv->glb + GLB_IRQ_MASK) & ~GLB_ETH_IRQ_BIT,
		       priv->glb + GLB_IRQ_MASK);
	}

	return work_done;
}

/* ---------------------------------------------------------------------
 * IRQ
 * ------------------------------------------------------------------- */

static irqreturn_t sd5113_eth_irq(int irq, void *data)
{
	struct sd5113_eth *priv = data;
	u32 cause = readl(priv->glb + GLB_IRQ_CAUSE);

	if (!(cause & GLB_ETH_IRQ_BIT))
		return IRQ_NONE;

	writel(readl(priv->glb + GLB_IRQ_MASK) | GLB_ETH_IRQ_BIT,
	       priv->glb + GLB_IRQ_MASK);

	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------
 * net_device_ops
 * ------------------------------------------------------------------- */

static int sd5113_eth_open(struct net_device *ndev)
{
	struct sd5113_eth *priv = netdev_priv(ndev);
	u32 val;
	int ret;

	netif_carrier_off(ndev);

	writel(readl(priv->cpurtx + CPURTX_FIFO_RESET) | BIT(0),
	       priv->cpurtx + CPURTX_FIFO_RESET);
	ret = readl_poll_timeout(priv->cpurtx + CPURTX_FIFO_RESET, val,
				  !(val & BIT(0)), 10, SD5113_POLL_TIMEOUT_US);
	if (ret) {
		netdev_err(ndev, "FIFO reset did not clear - hardware not responding?\n");
		return ret;
	}

	writel((u32)~GENMASK(SD5113_RX_NUM_SLOTS - 1, 0),
	       priv->cpurtx + CPURTX_CAUSE);

	/* CONFIRMED (round 30): hardware needs 128 pre-submitted buffers
	 * before it can receive anything at all - unlike round 29's driver,
	 * which never submitted any RX buffer. */
	sd5113_eth_fill_rx_pool(priv);

	napi_enable(&priv->napi);

	ret = request_irq(priv->irq, sd5113_eth_irq, IRQF_SHARED, DRV_NAME, priv);
	if (ret) {
		napi_disable(&priv->napi);
		sd5113_eth_drain_rx_pool(priv);
		return ret;
	}

	writel(readl(priv->glb + GLB_IRQ_MASK) & ~GLB_ETH_IRQ_BIT,
	       priv->glb + GLB_IRQ_MASK);

	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int sd5113_eth_stop(struct net_device *ndev)
{
	struct sd5113_eth *priv = netdev_priv(ndev);
	unsigned long flags;
	unsigned int ret;

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	writel(readl(priv->glb + GLB_IRQ_MASK) | GLB_ETH_IRQ_BIT,
	       priv->glb + GLB_IRQ_MASK);
	free_irq(priv->irq, priv);
	napi_disable(&priv->napi);

	sd5113_eth_tx_reclaim(priv);
	spin_lock_irqsave(&priv->lock, flags);
	for (ret = 0; ret < SD5113_TX_MAX_OUTSTANDING; ret++) {
		struct sk_buff *skb = priv->tx_buf[ret].skb;
		dma_addr_t dma_addr = priv->tx_buf[ret].dma_addr;

		priv->tx_buf[ret].skb = NULL;
		priv->tx_buf[ret].dma_addr = 0;
		if (skb) {
			priv->tx_count--;
			spin_unlock_irqrestore(&priv->lock, flags);
			dma_unmap_single(priv->dev, dma_addr, skb->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(skb);
			spin_lock_irqsave(&priv->lock, flags);
		}
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	sd5113_eth_drain_rx_pool(priv);
	return 0;
}

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
	atomic_set(&priv->dma_tx_map_logged, 0);
	atomic_set(&priv->dma_tx_done_logged, 0);
	atomic_set(&priv->dma_rx_map_logged, 0);

	/* CPURTx address registers are 32-bit. The vendor's `skb->data -
	 * 0x30000000` is consistent with the ARM linear-map VA->PA delta for
	 * RAM at 0x90000000; let the DMA API produce the device address and
	 * perform cache maintenance. Never apply that subtraction to dma_addr_t. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret,
				     "32-bit DMA is not available\n");

	/* reg[0] = glb block (0x42000010, 8 bytes), reg[1] = cpurtx block
	 * (0x42100000, 0xd8 bytes) - see hisilicon-sd5113-hg8245.dts. */
	priv->glb = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->glb))
		return PTR_ERR(priv->glb);

	priv->cpurtx = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->cpurtx))
		return PTR_ERR(priv->cpurtx);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	/* HYPOTHESIS: no MAC address source identified in this BSP beyond
	 * userspace jffs2cfg (not part of this archive) - fall back to a
	 * random locally-administered address. */
	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret)
		eth_hw_addr_random(ndev);

	ndev->netdev_ops = &sd5113_eth_netdev_ops;
	ndev->max_mtu = 1500;	/* CPURTX_MTU_CTRL/COUNT exist but their bit
				 * layout was never decoded - relying on
				 * power-on default rather than programming it. */

	netif_napi_add(ndev, &priv->napi, sd5113_eth_poll);

	writel(readl(priv->glb + GLB_IRQ_MASK) | GLB_ETH_IRQ_BIT,
	       priv->glb + GLB_IRQ_MASK);

	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&priv->napi);
		return ret;
	}

	platform_set_drvdata(pdev, ndev);
	netdev_info(ndev, "SD5113 CPU-port MAC (round 30, UNTESTED on real hardware, see driver header)\n");
	return 0;
}

static int sd5113_eth_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct sd5113_eth *priv = netdev_priv(ndev);

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
	},
};
module_platform_driver(sd5113_eth_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 CPU-port Ethernet MAC (reverse-engineered, untested)");
MODULE_LICENSE("GPL");

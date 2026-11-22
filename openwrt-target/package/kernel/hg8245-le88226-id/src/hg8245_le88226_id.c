// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

static bool tel_unplugged;
module_param(tel_unplugged, bool, 0400);
MODULE_PARM_DESC(tel_unplugged, "Confirm both TEL sockets are empty");

#define LE88226_MAX_GROUP	17
static DEFINE_MUTEX(id_lock);
static struct spi_device *deferred_spi;
static bool id_attempted;

static int le88226_byte(struct spi_device *spi, u8 *buf, bool read)
{
	struct spi_transfer transfer = {
		.tx_buf = read ? NULL : buf,
		.rx_buf = read ? buf : NULL,
		.len = 1,
	};
	struct spi_message message;
	int ret;

	spi_message_init_with_transfers(&message, &transfer, 1);
	ret = spi_sync_locked(spi, &message);
	return ret ?: (message.actual_length == 1 ? 0 : -EIO);
}

static int le88226_read_id_locked(struct spi_device *spi, u8 *buf, u8 id[2],
				  unsigned int *done)
{
	static const struct {
		u8 byte;
		u8 delay_us;
	} prefix[] = {
		[0] = { 0x4a, 0 }, [1] = { 0x01, 75 },
		[2] = { 0x06, 0 }, [3 ... 18] = { 0x06, 75 },
		[19] = { 0x4a, 0 }, [20] = { 0x01, 75 },
		[21] = { 0x73, 0 },
	};
	unsigned int i;
	int ret;

	*done = 0;
	for (i = 0; i < ARRAY_SIZE(prefix); i++) {
		if (prefix[i].delay_us)
			udelay(prefix[i].delay_us);
		*buf = prefix[i].byte;
		ret = le88226_byte(spi, buf, false);
		if (ret)
			return ret;
		++*done;
	}
	for (i = 0; i < 2; i++) {
		udelay(50);
		ret = le88226_byte(spi, buf, true);
		if (ret)
			return ret;
		id[i] = *buf;
		++*done;
	}
	return 0;
}

static int le88226_mpi_group(struct spi_device *spi, struct spi_transfer *xfers,
			     u8 *buf, u8 cmd, const u8 *payload, unsigned int writes,
			     unsigned int reads, unsigned int *done)
{
	unsigned int count = 1 + writes + reads;
	struct spi_message message;
	unsigned int i;
	int ret;

	if (count > LE88226_MAX_GROUP)
		return -EINVAL;
	memset(xfers, 0, count * sizeof(*xfers));
	buf[0] = cmd;
	if (writes)
		memcpy(buf + 1, payload, writes);

	for (i = 0; i < count; i++) {
		struct spi_transfer *xfer = &xfers[i];

		xfer->len = 1;
		if (i <= writes)
			xfer->tx_buf = buf + i;
		else
			xfer->rx_buf = buf + i;
		if (i + 1 < count) {
			xfer->cs_change = 1;
			xfer->cs_change_delay.unit = SPI_DELAY_UNIT_NSECS;
			xfer->delay.unit = SPI_DELAY_UNIT_USECS;
			xfer->delay.value = i < writes ? 75 : 50;
		}
	}
	spi_message_init_with_transfers(&message, xfers, count);
	ret = spi_sync_locked(spi, &message);
	*done += message.actual_length;
	return ret ?: (message.actual_length == count ? 0 : -EIO);
}

static int le88226_grouped_id(struct spi_device *spi, struct spi_transfer *xfers,
			      u8 *buf, u8 id[2], unsigned int *done)
{
	static const u8 ec = 1, sync[16] = { [0 ... 15] = 0x06 };
	int ret;

	*done = 0;
	ret = le88226_mpi_group(spi, xfers, buf, 0x4a, &ec, 1, 0, done);
	ret = ret ?: le88226_mpi_group(spi, xfers, buf, 0x06, sync, sizeof(sync), 0, done);
	ret = ret ?: le88226_mpi_group(spi, xfers, buf, 0x4a, &ec, 1, 0, done);
	ret = ret ?: le88226_mpi_group(spi, xfers, buf, 0x73, NULL, 0, 2, done);
	if (!ret) {
		id[0] = buf[1];
		id[1] = buf[2];
	}
	return ret;
}

static int hg8245_le88226_identify(struct spi_device *spi, bool grouped)
{
	struct spi_transfer *xfers = NULL;
	struct device *dev = &spi->dev;
	unsigned int done;
	u8 id[2], *buf;
	bool read_ec = grouped && of_property_read_bool(dev->of_node,
							"huawei,read-ec-after-id");
	int ec_ret = 0;
	int ret;

	buf = kmalloc(grouped ? LE88226_MAX_GROUP : 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (grouped) {
		xfers = kmalloc_array(LE88226_MAX_GROUP, sizeof(*xfers), GFP_KERNEL);
		if (!xfers) {
			ret = -ENOMEM;
			goto free;
		}
	}
	ret = spi_bus_lock(spi->controller);
	if (ret)
		goto free;
	ret = grouped ? le88226_grouped_id(spi, xfers, buf, id, &done) :
			le88226_read_id_locked(spi, buf, id, &done);
	if (!ret && read_ec) {
		unsigned int ec_done = 0;

		/* 0x4b reads back the EC selected with 0x4a before the ID. */
		ec_ret = le88226_mpi_group(spi, xfers, buf, 0x4b, NULL, 0, 1, &ec_done);
		if (ec_ret)
			dev_err(dev, "MPI EC read stopped after %u/2 bytes: %d\n", ec_done, ec_ret);
		else
			dev_info(dev, "MPI EC readback=%02x (last selected 01)\n", buf[1]);
	}
	spi_bus_unlock(spi->controller);
	if (ret) {
		dev_err(dev, "SPI ID exchange stopped after %u/24 bytes: %d\n", done, ret);
		goto free;
	}

	dev_info(dev, "LE88226 SPI core ID=%02x %02x (%s)\n", id[0], id[1],
		 (id[0] == 0x04 && id[1] == 0xb3) ? "matches stock diagnostic" :
						    "expected 04 b3");
	if (id[0] != 0x04 || id[1] != 0xb3)
		ret = -ENODEV;
	else if (ec_ret)
		ret = ec_ret;
free:
	kfree(xfers);
	kfree(buf);
	return ret;
}

int hg8245_le88226_run_id(void);
int hg8245_le88226_run_id(void)
{
	int ret;

	mutex_lock(&id_lock);
	if (!deferred_spi)
		ret = -ENODEV;
	else if (id_attempted)
		ret = -EALREADY;
	else {
		id_attempted = true;
		ret = hg8245_le88226_identify(deferred_spi, true);
	}
	mutex_unlock(&id_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hg8245_le88226_run_id);

static int hg8245_le88226_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	int ret = 0;

	if (!tel_unplugged ||
	    !of_machine_is_compatible("huawei,hg8245") ||
	    !of_device_is_compatible(dev->of_node, "huawei,hg8245-le88226-id") ||
	    !of_device_is_compatible(spi->controller->dev.of_node, "hisilicon,sd5113-spi") ||
	    spi_get_chipselect(spi, 0) != 0 || spi->mode != SPI_MODE_3 ||
	    spi->bits_per_word != 8 || spi->max_speed_hz != 8000000)
		return -ENODEV;
	if (!of_property_read_bool(dev->of_node, "huawei,wait-zsp"))
		return hg8245_le88226_identify(spi, false);

	mutex_lock(&id_lock);
	if (deferred_spi)
		ret = -EBUSY;
	else
		deferred_spi = spi;
	mutex_unlock(&id_lock);
	if (!ret)
		dev_info(dev, "SPI ID armed; no transfer until HIGHWAY and ZSP/TDM\n");
	return ret;
}

static void hg8245_le88226_remove(struct spi_device *spi)
{
	mutex_lock(&id_lock);
	if (deferred_spi == spi)
		deferred_spi = NULL;
	mutex_unlock(&id_lock);
}

static const struct of_device_id hg8245_le88226_of_match[] = {
	{ .compatible = "huawei,hg8245-le88226-id" },
	{ }
};

static const struct spi_device_id hg8245_le88226_ids[] = {
	{ "hg8245-le88226-id", 0 },
	{ }
};

static struct spi_driver hg8245_le88226_driver = {
	.driver = {
		.name = "hg8245-le88226-id",
		.of_match_table = hg8245_le88226_of_match,
	},
	.id_table = hg8245_le88226_ids,
	.probe = hg8245_le88226_probe,
	.remove = hg8245_le88226_remove,
};

static int __init hg8245_le88226_init(void)
{
	if (!tel_unplugged) {
		pr_err("hg8245-le88226-id: both TEL sockets must be empty; set tel_unplugged=1\n");
		return -EPERM;
	}
	return spi_register_driver(&hg8245_le88226_driver);
}
module_init(hg8245_le88226_init);

static void __exit hg8245_le88226_exit(void)
{
	spi_unregister_driver(&hg8245_le88226_driver);
}
module_exit(hg8245_le88226_exit);

MODULE_DESCRIPTION("HG8245 LE88226 ID-only SPI client");
MODULE_LICENSE("GPL");

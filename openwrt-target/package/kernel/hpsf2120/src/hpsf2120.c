// SPDX-License-Identifier: GPL-2.0-only
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <asm/unaligned.h>

/* Lower 128 bytes of A0 followed by the lower 128 bytes of A2. */
#define PAGE_LEN 128
/* HG8245V2/hw_ldsp_cfg.xml, OPTIC Cfg2..18. The first string is
 * the vendor-specific identification at A0:96, the second is the PN
 * at A0:40. Only the bias ADC offset differs between these DDMI maps.
 */
struct optic_profile {
	const char *vendor_part, *part, *hwmon;
	u8 bias_adc;
};

static const struct optic_profile profiles[] = {
	{ NULL, "HPSF2120", "hpsf2120", 0x64 },
	{ NULL, "RTXM167-407", "rtxm167_407", 0x64 },
	{ NULL, "RTXM167-407-C10", "rtxm167_407", 0x64 },
	{ "PTB4J85-85654-SC/APC-KHL+", NULL, "hg8245_ddm", 0x7c },
	{ "ITR-D3T-HW6", NULL, "hg8245_ddm", 0x6c },
	{ "PTB3J85-5638N-SC/APC-KNS+", "PTB3J85-5638N-S", "hg8245_ddm", 0x64 },
	{ "PTB3J85-5638N-SC/APC-KN+", "PTB3J85-5638N-S", "hg8245_ddm", 0x64 },
	{ "SFA-34-24T-HP-CDE-HW-3", NULL, "hg8245_ddm", 0x64 },
	{ "SFU-34-24T-HP-CDE-HW-4", "SFU3424THPCDEHW", "hg8245_ddm", 0x64 },
	{ NULL, "LTB34D3-CC+", "hg8245_ddm", 0x64 },
	{ "SFU-34-GB-P2-CDFB-HW", "SFU34GBP2CDFBHW", "hg8245_ddm", 0x64 },
	{ NULL, "RTXM169-504", "hg8245_ddm", 0x64 },
	{ NULL, "LTB3468L-CHG+", "hg8245_ddm", 0x64 },
	{ NULL, "LTB3468L-BHG+", "hg8245_ddm", 0x64 },
	{ NULL, "LTB3468L-CCG+", "hg8245_ddm", 0x64 },
	{ NULL, "LTB3468L-BCG+", "hg8245_ddm", 0x64 },
};
struct hpsf2120 {
	struct i2c_client *page[2];
	bool external_cal;
	u8 bias_adc;
};

static int hpsf_read(void *priv, unsigned int off, void *buf, size_t len)
{
	struct hpsf2120 *optic = priv;
	u8 *out = buf;
	int ret;

	if (off > 2 * PAGE_LEN || len > 2 * PAGE_LEN - off)
		return -EINVAL;
	while (len--) {
		struct i2c_client *c = optic->page[off / PAGE_LEN];
		u8 index = off % PAGE_LEN;
		struct i2c_msg msg[] = {
			{ .addr = c->addr, .len = 1, .buf = &index },
			{ .addr = c->addr, .flags = I2C_M_RD, .len = 1, .buf = out },
		};

		ret = i2c_transfer(c->adapter, msg, ARRAY_SIZE(msg));
		if (ret != ARRAY_SIZE(msg))
			return ret < 0 ? ret : -EIO;
		off++;
		out++;
	}
	return 0;
}

/* ADC and external slope/offset offsets from the HPSF212 board profile. */
static const struct hpsf_sensor {
	enum hwmon_sensor_types type;
	u8 adc, coeff;
	int numerator, denominator;
} sensors[] = {
	{ hwmon_temp,  0x60, 0x54, 1000, 256 },
	{ hwmon_in,    0x62, 0x58, 1, 10 },
	{ hwmon_curr,  0x64, 0x4c, 2, 1000 },
	{ hwmon_power, 0x66, 0x50, 1, 10 },
};

static int hpsf_sensor_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *value)
{
	struct hpsf2120 *optic = dev_get_drvdata(dev);
	const struct hpsf_sensor *sensor;
	u8 sample[2], high, coeff[4];
	s64 calibrated;
	int i, ret, raw;
	u8 adc;

	for (i = 0; i < ARRAY_SIZE(sensors); i++)
		if (type == sensors[i].type)
			break;
	if (i == ARRAY_SIZE(sensors) || channel)
		return -EOPNOTSUPP;
	sensor = &sensors[i];
	adc = type == hwmon_curr ? optic->bias_adc : sensor->adc;
	if (optic->external_cal)
		ret = hpsf_read(optic, PAGE_LEN + sensor->coeff, coeff, sizeof(coeff));
	else {
		put_unaligned_be16(256, coeff);
		put_unaligned_be16(0, coeff + 2);
		ret = 0;
	}
	ret = ret ?: hpsf_read(optic, PAGE_LEN + adc, sample, sizeof(sample));
	ret = ret ?: hpsf_read(optic, PAGE_LEN + adc, &high, 1);
	if (ret)
		return ret;
	if (high != sample[0])
		return -EAGAIN;
	if (!get_unaligned_be16(coeff))
		return -ENODATA;
	raw = get_unaligned_be16(sample);
	if (type == hwmon_temp)
		raw = (s16)raw;
	calibrated = (s64)raw * get_unaligned_be16(coeff) +
		     (s16)get_unaligned_be16(coeff + 2) * 256;
	if (type != hwmon_temp && calibrated < 0)
		return -ERANGE;
	*value = div_s64(calibrated * sensor->numerator, 256 * sensor->denominator);
	return 0;
}

static umode_t hpsf_visible(const void *data, enum hwmon_sensor_types type,
			    u32 attr, int channel)
{
	return 0444;
}

static const struct hwmon_ops hpsf_ops = {
	.is_visible = hpsf_visible, .read = hpsf_sensor_read,
};
static const struct hwmon_channel_info *hpsf_channels[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT),
	NULL,
};
static const struct hwmon_chip_info hpsf_hwmon = {
	.ops = &hpsf_ops, .info = hpsf_channels,
};

static int hpsf_probe(struct i2c_client *c)
{
	struct nvmem_config cfg = {
		.dev = &c->dev, .name = "hpsf2120", .id = NVMEM_DEVID_AUTO,
		.owner = THIS_MODULE, .size = 2 * PAGE_LEN, .word_size = 1,
		.stride = 1, .read_only = true, .reg_read = hpsf_read,
	};
	struct hpsf2120 *optic;
	char pn[17] = {}, vendor[33] = {};
	const struct optic_profile *profile = NULL;
	const char *model, *extension;
	unsigned int i;
	u8 diagnostic;
	int ret;

	if (c->addr != 0x50 || !i2c_check_functionality(c->adapter, I2C_FUNC_I2C))
		return -ENODEV;
	optic = devm_kzalloc(&c->dev, sizeof(*optic), GFP_KERNEL);
	if (!optic)
		return -ENOMEM;
	optic->page[0] = c;
	ret = hpsf_read(optic, 0x28, pn, sizeof(pn) - 1);
	if (ret)
		return ret;
	model = strim(pn);
	ret = hpsf_read(optic, 0x60, vendor, sizeof(vendor) - 1);
	extension = ret ? "" : strim(vendor);
	for (i = 0; i < ARRAY_SIZE(profiles); i++)
		if (profiles[i].vendor_part && !strcmp(extension, profiles[i].vendor_part)) {
			profile = &profiles[i];
			break;
		}
	if (!profile) {
		for (i = 0; i < ARRAY_SIZE(profiles); i++)
			if (profiles[i].part && !strcmp(model, profiles[i].part)) {
				profile = &profiles[i];
				break;
			}
	}
	cfg.name = profile ? profile->hwmon : "hg8245_optic";
	cfg.priv = optic;
	if (!profile) {
		cfg.size = PAGE_LEN;
		return PTR_ERR_OR_ZERO(devm_nvmem_register(&c->dev, &cfg));
	}
	optic->page[1] = devm_i2c_new_dummy_device(&c->dev, c->adapter, 0x51);
	if (IS_ERR(optic->page[1]))
		return PTR_ERR(optic->page[1]);
	optic->bias_adc = profile->bias_adc;
	ret = PTR_ERR_OR_ZERO(devm_nvmem_register(&c->dev, &cfg));
	if (ret)
		return ret;
	ret = hpsf_read(optic, 0x5c, &diagnostic, 1);
	if (ret)
		return ret;
	optic->external_cal = (diagnostic & 0x30) == 0x10;
	if (!optic->external_cal && (diagnostic & 0x30) != 0x20)
		return 0;
	return PTR_ERR_OR_ZERO(devm_hwmon_device_register_with_info(&c->dev,
			       profile->hwmon, optic, &hpsf_hwmon, NULL));
}

static const struct of_device_id hpsf_of[] = {
	{ .compatible = "huawei,hg8245v2-optics" },
	{ .compatible = "huawei,hpsf2120" }, {}
};
MODULE_DEVICE_TABLE(of, hpsf_of);

static struct i2c_driver hpsf_driver = {
	.driver = { .name = "hpsf2120", .of_match_table = hpsf_of },
	.probe = hpsf_probe,
};
module_i2c_driver(hpsf_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HPSF2120 optical transceiver identification and digital diagnostics");

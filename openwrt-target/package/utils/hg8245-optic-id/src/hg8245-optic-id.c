// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <glob.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int use_nvmem;
static int known_ddm;

static int adapter(unsigned int *bus)
{
	glob_t paths = { 0 };
	size_t matches = 0, i;

	if (glob("/sys/class/i2c-dev/i2c-*/name", 0, NULL, &paths))
		return -1;
	for (i = 0; i < paths.gl_pathc; i++) {
		const char *path = paths.gl_pathv[i];
		char name[64] = { 0 };
		unsigned int n;
		int end = 0;
		FILE *f = fopen(path, "r");

		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) &&
		    !strcmp(name, "SD5113 hardware I2C\n") &&
		    sscanf(path, "/sys/class/i2c-dev/i2c-%u/name%n", &n, &end) == 1 &&
		    end && !path[end]) {
			*bus = n;
			matches++;
		}
		fclose(f);
	}
	globfree(&paths);
	return matches == 1 ? 0 : -1;
}

static int read_byte(int fd, unsigned int addr, unsigned char offset, unsigned char *value)
{
	struct i2c_msg msgs[] = {
		{ .addr = addr, .len = 1, .buf = &offset },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = value },
	};
	struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };
	int ret;

	if (use_nvmem) {
		off_t pos = (addr == 0x51 ? 128 : 0) + offset;
		ssize_t n = pread(fd, value, 1, pos);

		if (n == 1)
			return 0;
		if (n < 0)
			perror("Optical nvmem");
		else
			fputs("Short HPSF2120 read.\n", stderr);
		return 1;
	}
	ret = ioctl(fd, I2C_RDWR, &xfer);

	if (ret == 2)
		return 0;
	fprintf(stderr, "Optical I2C 0x%02x offset 0x%02x: ", addr, offset);
	if (ret < 0)
		perror("I2C_RDWR");
	else
		fputs("short transfer\n", stderr);
	return 1;
}

static int rx_power(int fd, int external)
{
	unsigned char hi, lo, check, coeff[20];
	unsigned int i, raw;
	double power = 0;
	int has_calibration = 0;

	if (read_byte(fd, 0x51, 0x68, &hi) || read_byte(fd, 0x51, 0x69, &lo) ||
	    read_byte(fd, 0x51, 0x68, &check))
		return 1;
	if (hi != check) {
		fputs("RX sample changed during bytewise read.\n", stderr);
		return 1;
	}
	raw = (hi << 8) | lo;
	if (!external) {
		power = raw;
		goto report;
	}
	usleep(10000);
	for (i = 0; i < sizeof(coeff); i++) {
		if (read_byte(fd, 0x51, 0x38 + i, coeff + i))
			return 1;
		if (!(i % 4))
			usleep(10000);
	}
	/* HPSF212 profile: big-endian IEEE binary32, highest power first. */
	for (i = 0; i < sizeof(coeff); i += 4) {
		uint32_t bits = (uint32_t)coeff[i] << 24 | (uint32_t)coeff[i + 1] << 16 |
				(uint32_t)coeff[i + 2] << 8 | coeff[i + 3];
		float c;

		memcpy(&c, &bits, sizeof(c));
		if (!isfinite(c)) {
			fprintf(stderr, "RX raw=%u; invalid calibration coefficient.\n", raw);
			return 1;
		}
		has_calibration |= c != 0;
		power = power * raw + c;
	}
	if (!has_calibration || power < 0 || !isfinite(power)) {
		fprintf(stderr, "RX raw=%u; calibration unavailable.\n", raw);
		return 1;
	}
report:
	if (power == 0)
		printf("RX raw=%u, calibrated power=0 (no finite dBm)\n", raw);
	else
		printf("RX raw=%u, optical power=%.2f dBm\n", raw, 10 * log10(power / 10000));
	return 0;
}

static int read_optic(int fd, int power)
{
	unsigned char pn[16];
	unsigned int i;

	/* Stock reads the PN bytewise from 0x28..0x37 at address 0x50. */
	for (i = 0; i < sizeof(pn); i++) {
		if (read_byte(fd, 0x50, 0x28 + i, pn + i))
			return 1;
	}
	for (i = 0; i < sizeof(pn) && pn[i]; i++) {
		if (pn[i] < 0x20 || pn[i] > 0x7e) {
			fputs("Optical PN is not a printable EEPROM field.\n", stderr);
			return 1;
		}
	}
	while (i && pn[i - 1] == ' ')
		i--;
	if (!i && known_ddm) {
		unsigned char vendor[33] = {};

		for (i = 0; i < 32; i++) {
			if (read_byte(fd, 0x50, 96 + i, vendor + i))
				return 1;
			if (vendor[i] < 0x20 || vendor[i] > 0x7e)
				break;
		}
		if (i)
			printf("Optical vendor part: %.*s\n", (int)i, (char *)vendor);
		else
			return 1;
	} else if (!i) {
		fputs("Empty optical PN.\n", stderr);
		return 1;
	} else {
		printf("Optical PN: %.*s\n", (int)i, (const char *)pn);
	}
	if (power) {
		unsigned char mode;

		if (!known_ddm && !((i == 8 && !memcmp(pn, "HPSF2120", i)) ||
		      (i == 11 && !memcmp(pn, "RTXM167-407", i)) ||
		      (i == 15 && !memcmp(pn, "RTXM167-407-C10", i)))) {
			fputs("No RX diagnostic profile for this module.\n", stderr);
			return 1;
		}
		if (read_byte(fd, 0x50, 0x5c, &mode))
			return 1;
		if ((mode & 0x30) != 0x10 && (mode & 0x30) != 0x20) {
			fputs("EEPROM does not specify a unique calibration mode.\n", stderr);
			return 1;
		}
		return rx_power(fd, (mode & 0x30) == 0x10);
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int bus = 0;
	char device[48];
	int fd, ret, power = argc == 2 && !strcmp(argv[1], "--rx-power");
	const char *base = strrchr(argv[0], '/');

	base = base ? base + 1 : argv[0];
	use_nvmem = !strcmp(base, "hpsf2120") || !strcmp(base, "hg8245-optic");
	if (use_nvmem) {
		glob_t paths = { 0 };

		if (argc != 1 && !power) {
			fprintf(stderr, "Usage: %s [--rx-power]\n", argv[0]);
			return 2;
		}
		const char *patterns[] = {
			"/sys/bus/nvmem/devices/hpsf2120*/nvmem",
			"/sys/bus/nvmem/devices/rtxm167_407*/nvmem",
			"/sys/bus/nvmem/devices/hg8245_ddm*/nvmem",
			"/sys/bus/nvmem/devices/hg8245_optic*/nvmem",
		};
		unsigned int i;

		for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
			int found = glob(patterns[i], paths.gl_pathc ? GLOB_APPEND : 0, NULL, &paths);

			if (found && found != GLOB_NOMATCH) {
				globfree(&paths);
				return 1;
			}
		}
		if (paths.gl_pathc != 1) {
			fputs("Expected one optical nvmem provider.\n", stderr);
			globfree(&paths);
			return 1;
		}
		known_ddm = strstr(paths.gl_pathv[0], "/hg8245_optic") == NULL;
		fd = open(paths.gl_pathv[0], O_RDONLY | O_CLOEXEC);
		globfree(&paths);
		if (fd < 0) {
			perror("Optical nvmem");
			return 1;
		}
		ret = read_optic(fd, power);
		close(fd);
		return ret;
	}

	if ((argc != 1 && !power) || access("/sys/firmware/devicetree/base/huawei,board-io-lab", F_OK)) {
		fprintf(stderr, "Use %s [--rx-power] on the HG8245V2 board-I/O RAM profile.\n", argv[0]);
		return 2;
	}
	if (adapter(&bus)) {
		fputs("Expected exactly one SD5113 hardware I2C adapter; load the lab module first.\n", stderr);
		return 2;
	}
	snprintf(device, sizeof(device), "/dev/i2c-%u", bus);
	fd = open(device, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	ret = read_optic(fd, power);
	close(fd);
	return ret;
}

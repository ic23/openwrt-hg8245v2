"""Test the optic PN utility without touching sysfs or any I2C adapter."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "openwrt-target/package/utils/hg8245-optic-id/src/hg8245-optic-id.c"
HARNESS = r"""
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int ready = 1, good_adapter = 1, calls, fail_at = -1;
static char pn[] = "HPSF2120        ";
static unsigned char calibration_mode = 0x10;
static unsigned char sample[] = {0, 100, 0};
static unsigned char coeff[20] = { [12] = 0x3f, [13] = 0x80 }; /* x */
static const char namepath[] = "/sys/class/i2c-dev/i2c-3/name";

static int fake_access(const char *path, int how)
{
    assert(!strcmp(path, "/sys/firmware/devicetree/base/huawei,board-io-lab") && how == F_OK);
    return ready ? 0 : -1;
}

static int fake_glob(const char *pattern, int flags, int (*errfn)(const char *, int), glob_t *p)
{
    static char *v[] = {(char *)namepath, NULL};
    assert(!strcmp(pattern, "/sys/class/i2c-dev/i2c-*/name") && !flags && !errfn);
    p->gl_pathc = 1;
    p->gl_pathv = v;
    return 0;
}

static void fake_globfree(glob_t *p) { assert(p->gl_pathc == 1); }

static FILE *fake_fopen(const char *path, const char *mode)
{
    static char good[] = "SD5113 hardware I2C\n";
    static char bad[] = "not our bus\n";
    assert(!strcmp(path, namepath) && !strcmp(mode, "r"));
    return good_adapter ? fmemopen(good, sizeof(good) - 1, "r") :
                          fmemopen(bad, sizeof(bad) - 1, "r");
}

static int fake_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/i2c-3") && flags == (O_RDONLY | O_CLOEXEC));
    return 42;
}

static int fake_close(int fd) { assert(fd == 42); return 0; }

static int fake_ioctl(int fd, unsigned long cmd, ...)
{
    va_list ap;
    struct i2c_rdwr_ioctl_data *xfer;
    int at = calls++;

    assert(fd == 42 && cmd == I2C_RDWR && at < 40);
    va_start(ap, cmd);
    xfer = va_arg(ap, struct i2c_rdwr_ioctl_data *);
    va_end(ap);
    unsigned addr = at <= 16 ? 0x50 : 0x51;
    unsigned offset;
    if (at < 16)
        offset = 0x28 + at;
    else if (at == 16)
        offset = 0x5c;
    else if (at < 20)
        offset = 0x68 + (at == 18);
    else
        offset = 0x38 + at - 20;
    assert(xfer->nmsgs == 2 && xfer->msgs[0].addr == addr && xfer->msgs[1].addr == addr);
    assert(xfer->msgs[0].len == 1 && xfer->msgs[1].len == 1);
    assert(!xfer->msgs[0].flags && xfer->msgs[1].flags == I2C_M_RD);
    assert(xfer->msgs[0].buf[0] == offset);
    if (at == fail_at) {
        errno = ENXIO;
        return -1;
    }
    xfer->msgs[1].buf[0] = at < 16 ? (unsigned char)pn[at] :
                            at == 16 ? calibration_mode :
                            at < 20 ? sample[at - 17] : coeff[at - 20];
    return 2;
}

#define main optic_main
#define access fake_access
#define glob fake_glob
#define globfree fake_globfree
#define fopen fake_fopen
#define open fake_open
#define close fake_close
#define ioctl fake_ioctl
#include "optic.c"
#undef main

int main(void)
{
    char *argv[] = {"hg8245-optic-id", NULL};
    ready = 0;
    assert(optic_main(1, argv) == 2 && !calls);
    ready = 1;
    good_adapter = 0;
    assert(optic_main(1, argv) == 2 && !calls);
    good_adapter = 1;
    fail_at = 3;
    assert(optic_main(1, argv) == 1 && calls == 4);
    fail_at = -1;
    calls = 0;
    assert(optic_main(1, argv) == 0 && calls == 16);
    char *rx[] = {"hg8245-optic-id", "--rx-power", NULL};
    for (fail_at = 0; fail_at < 40; fail_at++) {
        calls = 0;
        assert(optic_main(2, rx) == 1 && calls == fail_at + 1);
    }
    fail_at = -1;
    calls = 0;
    pn[7] = '1';
    assert(optic_main(2, rx) == 1 && calls == 16);
    pn[7] = '0';
    calls = 0;
    calibration_mode = 0;
    assert(optic_main(2, rx) == 1 && calls == 17);
    calibration_mode = 0x20;
    calls = 0;
    assert(optic_main(2, rx) == 0 && calls == 20);
    calibration_mode = 0x10;
    calls = 0;
    sample[2] = 1;
    assert(optic_main(2, rx) == 1 && calls == 20);
    calls = sample[2] = 0;
    assert(optic_main(2, rx) == 0 && calls == 40);
    calls = 0;
    coeff[0] = 0x7f; coeff[1] = 0xc0; /* NaN */
    assert(optic_main(2, rx) == 1 && calls == 40);
    memset(coeff, 0, sizeof(coeff));
    calls = 0;
    assert(optic_main(2, rx) == 1 && calls == 40);
    coeff[0] = 0x3f; coeff[1] = 0x80; /* x^4 */
    calls = 0;
    assert(optic_main(2, rx) == 0 && calls == 40);
    sample[1] = 0;
    calls = 0;
    assert(optic_main(2, rx) == 0 && calls == 40);
}
"""

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hg8245-optic-id-") as path:
        d = Path(path)
        (d / "optic.c").write_bytes(SOURCE.read_bytes())
        (d / "test.c").write_text(HARNESS, encoding="utf-8")
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "test.c", "-o", "test", "-lm"], cwd=d, check=True)
        output = subprocess.check_output([str(d / "test")], cwd=d, timeout=5,
                                         text=True, stderr=subprocess.PIPE)
        assert output.startswith("Optical PN: HPSF2120\n"), output
        assert [line for line in output.splitlines() if line.startswith("RX")] == [
            "RX raw=100, optical power=-20.00 dBm",  # internal calibration
            "RX raw=100, optical power=-20.00 dBm",  # external calibration
            "RX raw=100, optical power=40.00 dBm",
            "RX raw=0, calibrated power=0 (no finite dBm)",
        ], output
    print("PASS: PN and RX reads, address bounds, calibration, changed sample and NACK stop")

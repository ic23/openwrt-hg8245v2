"""Compile the real SD5113 I2C controller and transfer paths with host mocks.

Run under Linux/WSL: python3 tools/test_i2c_sd5113.py [--cc cc]
The test models counters and MMIO, not DesignWare timing or electrical behavior.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[1] /
          "openwrt-target/target/linux/sd5113/files-6.6/drivers/i2c/busses/i2c-sd5113.c")

PRELUDE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define __iomem
#define U16_MAX UINT16_MAX
#define BIT(x) (1U << (x))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
#define dev_warn(...) ((void)0)
#define I2C_M_RD 0x0001
#define I2C_M_DMA_SAFE 0x0200
#define I2C_FUNC_I2C 1
struct device { int unused; };
struct clk { int unused; };
struct gpio_desc { int unused; };
struct gpio_descs { unsigned int ndescs; struct gpio_desc *desc[2]; };
struct regmap { u32 route, gate; };
struct i2c_adapter { void *data; };
struct i2c_adapter_quirks { int unused; };
struct i2c_msg { u16 addr, flags, len; u8 *buf; };
struct i2c_algorithm {
    int (*master_xfer)(struct i2c_adapter *, struct i2c_msg *, int);
    u32 (*functionality)(struct i2c_adapter *);
};
#define i2c_get_adapdata(a) ((a)->data)

/* Independent model constants, including register roles not in the ROM. */
enum { CON = 0, TAR = 4, DATA = 0x10, HCNT = 0x14, LCNT = 0x18,
       INTR_MASK = 0x30, RAW = 0x34, CLR = 0x40, ENABLE = 0x6c, STATUS = 0x70,
       RXFLR = 0x78, ABRT_SRC = 0x80, EN_STATUS = 0x9c, PARAM = 0xf4, TYPE = 0xfc };
enum { STOP_DET = 0x200, TX_ABORT = 0x40, FIFO_ERROR = 8 };
enum { GOOD, TIMEOUT, ADDR_NACK, DATA_NACK, ARB_LOST, SHORT_RX, FIFO_ERR, DISABLE_STUCK };
static struct {
    unsigned char mmio[0x100];
    u32 regs[0x100 / 4];
    struct regmap crg;
    struct gpio_descs pads;
    struct clk clk;
    struct device dev;
    unsigned int func, commands[64], num_commands, num_reads, read_pos, scenario;
    unsigned int accesses, polls, aborts, clock_enables, clock_disables;
    bool irq_locked, gate_broken, busy;
} hw;

static unsigned int offset(const void *addr)
{
    ptrdiff_t n = (const unsigned char *)addr - hw.mmio;
    assert(hw.crg.gate & (1U << 27));
    assert(n >= 0 && n < 0x100 && n % 4 == 0);
    hw.accesses++;
    return n;
}

static u32 readl(const void *addr)
{
    unsigned int reg = offset(addr);
    assert(!hw.irq_locked);
    switch (reg) {
    case CLR:
        hw.regs[RAW / 4] = hw.regs[ABRT_SRC / 4] = 0;
        return 0;
    case STATUS:
        return 4 | (hw.busy ? 0x21 : 0) | (hw.read_pos < hw.num_reads ? 8 : 0);
    case RXFLR:
        return hw.num_reads - hw.read_pos;
    case DATA:
        assert(hw.read_pos < hw.num_reads);
        return 0xab00 | (0x41 + hw.read_pos++);
    default:
        return hw.regs[reg / 4];
    }
}

static void writel(u32 val, void *addr)
{
    unsigned int reg = offset(addr);
    assert(!hw.irq_locked);
    switch (reg) {
    case TAR:
    case INTR_MASK:
    case CON:
    case HCNT:
    case LCNT:
        assert(!hw.regs[EN_STATUS / 4]);
        break;
    case ENABLE:
        if (val == 3) {
            assert(hw.busy);
            hw.aborts++;
            hw.busy = false;
            hw.regs[RAW / 4] = TX_ABORT;
            val = 1;
        }
        if (!val && hw.scenario == DISABLE_STUCK && hw.num_commands)
            return;
        if (!val) {
            hw.num_reads = hw.read_pos = 0;
            hw.busy = false;
        }
        hw.regs[EN_STATUS / 4] = val;
        break;
    default:
        assert(0);
    }
    hw.regs[reg / 4] = val;
}

static void writel_relaxed(u32 val, void *addr)
{
    assert(offset(addr) == DATA);
    assert(hw.irq_locked && hw.regs[EN_STATUS / 4]);
    assert((hw.crg.route & (1U << 22)) && (hw.func & 0x60) == 0x60);
    assert(hw.num_commands < sizeof(hw.commands) / sizeof(hw.commands[0]));
    assert(!hw.num_commands || !(hw.commands[hw.num_commands - 1] & 0x200));
    hw.commands[hw.num_commands++] = val;
    if (val & 0x100)
        hw.num_reads++;
    if (!(val & 0x200))
        return;
    switch (hw.scenario) {
    case TIMEOUT: hw.busy = true; return;
    case ADDR_NACK: hw.regs[ABRT_SRC / 4] = 1; break;
    case DATA_NACK: hw.regs[ABRT_SRC / 4] = 8; break;
    case ARB_LOST: hw.regs[ABRT_SRC / 4] = 0x1000; break;
    case SHORT_RX: hw.num_reads--; break;
    case FIFO_ERR: hw.regs[RAW / 4] = FIFO_ERROR; return;
    }
    hw.regs[RAW / 4] = STOP_DET | (hw.regs[ABRT_SRC / 4] ? TX_ABORT : 0);
}

/* Logical time, not sleeps. Ensure a nonzero retry interval and deadline. */
#define readl_poll_timeout(addr, val, ok, delay, end) ({ \
    unsigned int t; \
    int rc = -ETIMEDOUT; \
    assert((delay) > 0 && (end) <= 26000); \
    for (t = 0; t <= (end); t += (delay)) { \
        hw.polls++; val = readl(addr); \
        if (ok) { rc = 0; break; } \
    } \
    rc; \
})
#define local_irq_save(f) do { assert(!hw.irq_locked); hw.irq_locked = 1; (f) = 1; } while (0)
#define local_irq_restore(f) do { assert((f) && hw.irq_locked); hw.irq_locked = 0; } while (0)

static int clk_prepare_enable(struct clk *clk)
{
    assert(clk == &hw.clk);
    hw.clock_enables++;
    if (!hw.gate_broken)
        hw.crg.gate |= 1U << 27;
    return 0;
}
static void clk_disable_unprepare(struct clk *clk)
{
    assert(clk == &hw.clk);
    hw.clock_disables++;
    hw.crg.gate &= ~(1U << 27);
}
static int regmap_read(struct regmap *map, unsigned int reg, u32 *val)
{
    assert(map == &hw.crg && (reg == 0x1c || reg == 0x2c));
    *val = reg == 0x1c ? map->route : map->gate;
    return 0;
}
static int regmap_update_bits(struct regmap *map, unsigned int reg, u32 mask, u32 val)
{
    assert(map == &hw.crg && reg == 0x1c && mask == 1U << 22);
    map->route = (map->route & ~mask) | (val & mask);
    return 0;
}
#define regmap_clear_bits(map, reg, mask) regmap_update_bits(map, reg, mask, 0)
#define regmap_set_bits(map, reg, mask) regmap_update_bits(map, reg, mask, mask)
static int sd5113_gpio_set_funcs(struct gpio_desc *const *pads, unsigned int count,
                                u32 mask, u32 value, u32 *old)
{
    assert(pads == hw.pads.desc && count == 2);
    assert(!mask || mask == 0x60);
    if (old)
        *old = hw.func & 0x60;
    hw.func = (hw.func & ~mask) | (value & mask);
    return 0;
}
"""

TESTS = r"""
static void init(struct sd5113_i2c *i2c, u32 param)
{
    memset(&hw, 0, sizeof(hw));
    *i2c = (struct sd5113_i2c){
        .dev = &hw.dev, .base = hw.mmio, .clk = &hw.clk, .crg = &hw.crg, .pads = &hw.pads,
        .adap.data = i2c,
    };
    hw.crg.route = 0x00e00001;
    hw.crg.gate = 1U << 26;
    hw.pads.ndescs = 2;
    hw.func = 0xaf;
    hw.regs[TYPE / 4] = 0x44570140;
    hw.regs[PARAM / 4] = param;
    hw.regs[HCNT / 4] = 0xabcd1234;
    hw.regs[LCNT / 4] = 0x43211234;
}

static void assert_unwound(void)
{
    assert(hw.crg.route == 0x00e00001 && hw.crg.gate == (1U << 26) && hw.func == 0xaf);
    assert(hw.clock_enables == 1 && hw.clock_disables == 1);
}

static void next_xfer(unsigned int scenario)
{
    hw.num_commands = hw.num_reads = hw.read_pos = 0;
    hw.scenario = scenario;
}

int main(void)
{
    struct sd5113_i2c i2c;
    u32 count = 0;
    u8 tx[] = {0x28, 0x29, 0x2a, 0x2b, 0x2c}, rx[5] = {0};
    struct i2c_msg combo[2] = {
        {.addr = 0x50, .len = 1, .buf = tx},
        {.addr = 0x50, .flags = I2C_M_RD | I2C_M_DMA_SAFE, .len = 2, .buf = rx},
    };
    unsigned int accesses;

    assert(sd_i2c_divisor(0, &count) == -ERANGE);
    assert(!sd_i2c_divisor(133250000, &count) && count == 667);
    init(&i2c, 0x070380); /* TX 8, RX 4 */
    assert(sd_i2c_start(&i2c, count) == 0);
    assert(hw.crg.route == 0x00e00001 && hw.func == 0xef);
    assert(hw.regs[INTR_MASK / 4] == 0 && hw.regs[CON / 4] == 0x63);
    assert(hw.regs[HCNT / 4] == 0xabcd029b && hw.regs[LCNT / 4] == 0x4321029b);
    assert(i2c.tx_depth == 8 && i2c.rx_depth == 4);
    assert(!hw.num_commands && sd_i2c_algorithm.functionality(&i2c.adap) == 1);

    assert(sd_i2c_algorithm.master_xfer(&i2c.adap, combo, 2) == 2);
    assert(hw.regs[TAR / 4] == 0x50 && !hw.regs[EN_STATUS / 4]);
    assert(hw.num_commands == 3);
    assert(hw.commands[0] == 0x28 && hw.commands[1] == 0x500 && hw.commands[2] == 0x300);
    assert(rx[0] == 'A' && rx[1] == 'B');

    next_xfer(GOOD);
    assert(sd_i2c_xfer(&i2c.adap, combo + 1, 1) == 1);
    assert(hw.num_commands == 2 && hw.commands[0] == 0x100 && hw.commands[1] == 0x300);
    next_xfer(GOOD);
    assert(sd_i2c_xfer(&i2c.adap, combo, 1) == 1);
    assert(hw.num_commands == 1 && hw.commands[0] == 0x228);

    accesses = hw.accesses;
    combo[1].len = 5;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EOPNOTSUPP);
    combo[1].len = 2;
    combo[0].len = 7;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EOPNOTSUPP);
    combo[0].len = 1;
    combo[1].addr = 0x51;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EOPNOTSUPP);
    combo[1].addr = 0x50;
    combo[0].flags = 0x0010;
    assert(sd_i2c_xfer(&i2c.adap, combo, 1) == -EOPNOTSUPP);
    combo[0].flags = 0;
    combo[0].len = 0;
    assert(sd_i2c_xfer(&i2c.adap, combo, 1) == -EOPNOTSUPP);
    combo[0].len = 1;
    assert(hw.accesses == accesses);

    next_xfer(GOOD);
    hw.busy = true;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -ETIMEDOUT);
    assert(!hw.num_commands && !hw.aborts);
    hw.busy = false;
    next_xfer(TIMEOUT);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -ETIMEDOUT);
    assert(hw.aborts == 1 && !hw.regs[EN_STATUS / 4] && hw.polls < 5000);
    next_xfer(ADDR_NACK);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -ENXIO);
    next_xfer(DATA_NACK);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EREMOTEIO);
    next_xfer(ARB_LOST);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EAGAIN && hw.aborts == 1);
    next_xfer(FIFO_ERR);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EIO);
    next_xfer(SHORT_RX);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EIO);
    next_xfer(GOOD);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == 2 && !i2c.failed);
    next_xfer(DISABLE_STUCK);
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -ETIMEDOUT && i2c.failed);
    accesses = hw.accesses;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == -EIO && hw.accesses == accesses);
    next_xfer(GOOD);
    sd_i2c_shutdown(&i2c);
    assert_unwound();

    init(&i2c, 0);
    assert(sd_i2c_start(&i2c, count) == 0 && i2c.tx_depth == 2 && i2c.rx_depth == 2);
    combo[1].len = 1;
    assert(sd_i2c_xfer(&i2c.adap, combo, 2) == 2);
    assert(hw.num_commands == 2 && hw.commands[1] == 0x700);
    sd_i2c_shutdown(&i2c);
    assert_unwound();

    init(&i2c, 0);
    hw.regs[TYPE / 4] = 0;
    assert(sd_i2c_start(&i2c, count) == -ENODEV && hw.accesses == 1);
    sd_i2c_shutdown(&i2c);
    assert(hw.accesses == 1);
    assert_unwound();
    init(&i2c, 0);
    hw.gate_broken = true;
    assert(sd_i2c_start(&i2c, count) == -EIO && !hw.accesses);
    sd_i2c_shutdown(&i2c);
    assert_unwound();
    puts("PASS: native SD5113 I2C register, command, limit, error, timeout and unwind paths");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = SOURCE.read_text()
    source = source[source.index("#define SD_I2C_CON"):source.index("static int sd_i2c_probe")]
    with tempfile.TemporaryDirectory() as tmp:
        c_path, exe = Path(tmp) / "test.c", Path(tmp) / "test-i2c"
        c_path.write_text(PRELUDE + source + TESTS)
        subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-sign-compare", str(c_path), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()

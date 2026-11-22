"""Compile the actual SD5113 SPI driver helpers against deterministic host mocks.

Run in WSL/Linux: python3 tools/test_spi_sd5113.py --cc cc
The model checks register transactions, not Linux SPI core or hardware timing.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[1] /
          'openwrt-target/target/linux/sd5113/files-6.6/drivers/spi/spi-sd5113.c')

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define round_up(n, a) ((((n) - 1) | ((__typeof__(n))(a) - 1)) + 1)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* The mock's register addresses and masks are independent of driver defines. */
enum { FORMAT = 0, ENABLE = 8, SELECT = 0x10, DIV = 0x14,
       STATUS = 0x28, IRQ = 0x2c, DATA = 0x60 };
enum { CTRL = 0x1c, SET = 0x24, CLEAR = 0x28, GATE_STATUS = 0x2c, ROUTE = 0x40 };
enum { GATE = 1U << 26, MUX = 1U << 21 };
enum { SPI_WRITE, CRG_WRITE, GPIO_WRITE, FIFO_READ };
struct event { unsigned kind, offset; u32 value; };
struct regmap { u32 ctrl, route, status; };
struct gpio_desc { int unused; };
struct gpio_descs { u32 funcs; unsigned int ndescs; struct gpio_desc *desc[4]; };
struct device {
    void (*cleanup)(void *);
    void *cleanup_data;
    unsigned warnings;
};
struct list_head { struct list_head *next, *prev; };
struct spi_transfer {
    struct list_head transfer_list;
    unsigned int len, speed_hz;
    const void *tx_buf;
    void *rx_buf;
    bool cs_off, cs_change;
};
struct spi_message { struct list_head transfers; };
struct spi_controller { void *priv; };
#define spi_controller_get_devdata(ctlr) ((ctlr)->priv)
#define list_empty(head) ((head)->next == (head))
#define list_is_last(node, head) ((node)->next == (head))
#define list_entry(node, type, member) \
    ((type *)((char *)(node) - offsetof(type, member)))
#define list_first_entry(head, type, member) list_entry((head)->next, type, member)
#define list_for_each_entry(pos, head, member) \
    for (struct list_head *cur = (head)->next; \
         cur != (head) && ((pos) = list_entry(cur, __typeof__(*(pos)), member), true); \
         cur = cur->next)
static struct {
    unsigned char mmio[0x64];
    u32 spi[0x64 / 4];
    struct regmap crg;
    struct gpio_descs pads;
    struct gpio_desc first_pad;
    struct device dev;
    struct event events[32];
    size_t count;
    const u32 *samples;
    size_t sample_count, status_reads;
    u32 status_tail, last_status, reply;
    bool drop_select_write;
} hw;

static void record(unsigned kind, unsigned offset, u32 value)
{
    assert(hw.count < ARRAY_SIZE(hw.events));
    hw.events[hw.count++] = (struct event){kind, offset, value};
}

static unsigned spi_offset(const void *addr)
{
    ptrdiff_t offset = (const unsigned char *)addr - hw.mmio;

    assert(hw.crg.status & GATE);  /* No access to a clock-gated SPI block. */
    assert(offset >= 0 && (size_t)offset < sizeof(hw.mmio) && !(offset % 4));
    return (unsigned)offset;
}

static u32 readl(const void *addr)
{
    unsigned offset = spi_offset(addr);

    switch (offset) {
    case STATUS: {
        size_t n = hw.status_reads++;
        return hw.last_status = n < hw.sample_count ? hw.samples[n] : hw.status_tail;
    }
    case DATA:
        assert((hw.spi[ENABLE / 4] & 1) && (hw.spi[FORMAT / 4] & 0x300) == 0x200);
        assert(hw.last_status & 8);
        record(FIFO_READ, offset, hw.reply);
        return hw.reply;
    default:
        return hw.spi[offset / 4];
    }
}

static void writel(u32 value, void *addr)
{
    unsigned offset = spi_offset(addr);

    record(SPI_WRITE, offset, value);
    if (offset == DATA) {
        assert(hw.spi[ENABLE / 4] & 1);
        if ((hw.spi[FORMAT / 4] & 0x300) == 0x100)
            assert(hw.last_status & 2);
        else
            assert((hw.spi[FORMAT / 4] & 0x300) == 0x200 && value == 0xffff);
    } else if (offset == SELECT && hw.drop_select_write) {
        hw.drop_select_write = false;
        return;
    }
    hw.spi[offset / 4] = value;
}

static u32 *crg_register(struct regmap *map, unsigned offset)
{
    assert(map == &hw.crg);
    switch (offset) {
    case CTRL: return &map->ctrl;
    case ROUTE: return &map->route;
    case GATE_STATUS: return &map->status;
    default: assert(!"unexpected CRG read/RMW");
    }
    return NULL;
}

static int regmap_read(struct regmap *map, unsigned offset, u32 *value)
{
    *value = *crg_register(map, offset);
    return 0;
}

static int regmap_write(struct regmap *map, unsigned offset, u32 value)
{
    assert(map == &hw.crg && value == GATE && (offset == SET || offset == CLEAR));
    record(CRG_WRITE, offset, value);
    if (offset == SET)
        map->status |= value;
    else
        map->status &= ~value;
    return 0;
}

static int regmap_update_bits(struct regmap *map, unsigned offset, u32 mask, u32 value)
{
    u32 *reg = crg_register(map, offset);
    u32 next = (*reg & ~mask) | (value & mask);

    assert(offset == CTRL || offset == ROUTE);
    if (next != *reg) {
        *reg = next;
        record(CRG_WRITE, offset, next);
    }
    return 0;
}
#define regmap_clear_bits(map, reg, mask) regmap_update_bits(map, reg, mask, 0)
#define regmap_set_bits(map, reg, mask) regmap_update_bits(map, reg, mask, mask)

/* Deterministic polling budget; evaluate the driver's condition on every read. */
#define POLL_LIMIT 8
#define MOCK_POLL(read_op, condition)                                        \
    ({                                                                      \
        int poll_rc = -ETIMEDOUT;                                           \
        for (unsigned attempt = 0; attempt < POLL_LIMIT; ++attempt) {       \
            int read_rc = (read_op);                                       \
            if (read_rc || (condition)) {                                  \
                poll_rc = read_rc;                                         \
                break;                                                     \
            }                                                              \
        }                                                                  \
        poll_rc;                                                           \
    })
#define readl_poll_timeout_atomic(addr, val, cond, delay, timeout) \
    MOCK_POLL(((val) = readl(addr), 0), cond)
#define regmap_read_poll_timeout(map, reg, val, cond, delay, timeout) \
    MOCK_POLL(regmap_read(map, reg, &(val)), cond)

static int sd5113_gpio_set_funcs(struct gpio_desc * const *pads, unsigned int count,
                                u32 mask, u32 value, u32 *old)
{
    assert(pads == hw.pads.desc && count == 4 && !(mask & ~0x0fU));
    if (old)
        *old = hw.pads.funcs & 0x0f;
    if (mask) {
        hw.pads.funcs = (hw.pads.funcs & ~mask) | (value & mask);
        record(GPIO_WRITE, 8, hw.pads.funcs);
    }
    return 0;
}

static int sd5113_gpio_read_bank_funcs(struct gpio_desc *pad, u32 *funcs)
{
    assert(pad == &hw.first_pad);
    *funcs = hw.pads.funcs;
    return 0;
}

static int devm_add_action(struct device *dev, void (*cleanup)(void *), void *data)
{
    assert(dev == &hw.dev && !dev->cleanup);
    dev->cleanup = cleanup;
    dev->cleanup_data = data;
    return 0;
}

static void dev_warn(struct device *dev, const char *message, ...)
{
    va_list args;
    ++dev->warnings;
    va_start(args, message);
    vfprintf(stderr, message, args);
    va_end(args);
}
'''

TESTS = r'''
static struct sd5113_spi reset(bool clock_on)
{
    hw = (__typeof__(hw)){0};
    hw.crg = (struct regmap){0x40200010, 0x00600100, 0x08U | (clock_on ? GATE : 0)};
    hw.pads.funcs = 0xa4;
    hw.pads.ndescs = 4;
    hw.pads.desc[0] = &hw.first_pad;
    hw.spi[ENABLE / 4] = 0x40;
    hw.spi[FORMAT / 4] = 0x80004a15;
    hw.spi[SELECT / 4] = 0xface0002;
    hw.spi[DIV / 4] = 0x155a0094;
    hw.spi[IRQ / 4] = 0x1000003f;
    hw.reply = 0x1234ab5e;
    return (struct sd5113_spi){
        .base = hw.mmio, .crg = &hw.crg, .pads = &hw.pads, .dev = &hw.dev,
    };
}

static void expect_events(const struct event *expected, size_t count)
{
    assert(hw.count == count);
    for (size_t i = 0; i < count; ++i) {
        struct event a = hw.events[i], b = expected[i];

        if (a.kind != b.kind || a.offset != b.offset || a.value != b.value) {
            fprintf(stderr, "event %zu: got (%u,%#x,%#x), expected (%u,%#x,%#x)\n",
                    i, a.kind, a.offset, a.value, b.kind, b.offset, b.value);
            assert(!"register event mismatch");
        }
    }
}

static void test_divisor(void)
{
    const struct { unsigned long ahb; u32 request, div; int rc; } cases[] = {
        {128000000, 8000000, 16, 0},    /* Exact even quotient. */
        {120000000, 8000000, 16, 0},    /* Exact odd quotient. */
        {133000000, 8000000, 18, 0},    /* Even floor would overspeed. */
        {133000000, 7400000, 18, 0},
        {133250000, 7400000, 20, 0},
        {8000000, 8000000, 2, 0},
        {65534UL * 2000, 2000, 65534, 0},
        {65534UL * 2000 + 1, 2000, 0, -ERANGE},
        {0, 8000000, 0, -ERANGE},
        {133250000, 0, 0, -EINVAL},
        {133250000, 8000001, 0, -EINVAL},
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        u32 div = UINT32_MAX;

        assert(sd_spi_divisor(cases[i].ahb, cases[i].request, &div) == cases[i].rc);
        assert(div == (cases[i].rc ? UINT32_MAX : cases[i].div));
        if (!cases[i].rc)
            assert(cases[i].ahb <= (unsigned long)div * cases[i].request);
    }
}

static void test_bytes(void)
{
    static const u32 delayed_tx[] = {0x4, 0x8, 0x2, 0x5, 0x2, 0x4};
    static const u32 delayed_rx[] = {0x6, 0x7, 0xe};
    static const u32 tx_ready[] = {0x2};
    const struct {
        bool receive;
        const u32 *samples;
        size_t nsamples;
        u32 tail;
        int rc;
        size_t reads;
        bool fifo_write;
    } cases[] = {
        {false, delayed_tx, ARRAY_SIZE(delayed_tx), 0, 0, 6, true},
        {true, delayed_rx, ARRAY_SIZE(delayed_rx), 0, 0, 3, true},
        {false, NULL, 0, 0x4, -ETIMEDOUT, POLL_LIMIT, false},
        {false, tx_ready, ARRAY_SIZE(tx_ready), 0x5, -ETIMEDOUT, 1 + POLL_LIMIT, true},
        {true, NULL, 0, 0x6, -ETIMEDOUT, POLL_LIMIT, true},
    };
    const u8 tx = 0x73;

    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        struct sd5113_spi ss = reset(true);
        bool rx_ok = cases[i].receive && !cases[i].rc;
        u8 rx = 0xa5;
        struct event expected[5] = {
            {SPI_WRITE, FORMAT, cases[i].receive ? 0x80004a15 : 0x80004915},
            {SPI_WRITE, ENABLE, 0x41},
        };
        size_t n = 2;

        if (cases[i].receive)
            hw.spi[FORMAT / 4] = 0x80004915;  /* Switch from TX to RX. */
        hw.samples = cases[i].samples;
        hw.sample_count = cases[i].nsamples;
        hw.status_tail = cases[i].tail;
        assert(sd_spi_byte(&ss, cases[i].receive ? NULL : &tx,
                          cases[i].receive ? &rx : NULL) == cases[i].rc);
        if (cases[i].fifo_write)
            expected[n++] = (struct event){SPI_WRITE, DATA, cases[i].receive ? 0xffff : tx};
        if (rx_ok)
            expected[n++] = (struct event){FIFO_READ, DATA, 0x1234ab5e};
        expected[n++] = (struct event){SPI_WRITE, ENABLE, 0x40};
        expect_events(expected, n);
        assert(hw.status_reads == cases[i].reads);
        assert(rx == (rx_ok ? 0x5e : 0xa5) && hw.spi[ENABLE / 4] == 0x40);
    }

    struct sd5113_spi ss = reset(true);
    u8 rx = 0xa5;
    assert(sd_spi_byte(&ss, NULL, NULL) == -EINVAL);
    assert(sd_spi_byte(&ss, &tx, &rx) == -EINVAL);
    assert(hw.count == 0 && hw.status_reads == 0 && rx == 0xa5);
}

static void cleanup(struct sd5113_spi *ss, u32 ctrl, u32 route)
{
    assert(hw.dev.cleanup && hw.dev.cleanup_data == ss);
    hw.dev.cleanup(hw.dev.cleanup_data);
    assert(!hw.dev.warnings);
    assert(hw.crg.ctrl == ctrl && hw.crg.route == route);
    assert(hw.pads.funcs == 0xa4 && hw.spi[ENABLE / 4] == 0x40);
}

static void assert_spi_snapshot(void)
{
    assert(hw.crg.status == (0x08U | GATE));
    assert(hw.spi[FORMAT / 4] == 0x80004a15);
    assert(hw.spi[SELECT / 4] == 0xface0002);
    assert(hw.spi[DIV / 4] == 0x155a0094);
    assert(hw.spi[IRQ / 4] == 0x1000003f);
}

static void test_start_restore(void)
{
    static const struct event start[] = {
        {CRG_WRITE, CLEAR, GATE},
        {CRG_WRITE, CTRL, 0x40000010},
        {GPIO_WRITE, 8, 0xac},
        {CRG_WRITE, ROUTE, 0x00400100},
        {GPIO_WRITE, 8, 0xa0},
        {CRG_WRITE, CLEAR, GATE},
        {GPIO_WRITE, 8, 0xa8},
        {CRG_WRITE, ROUTE, 0x00600100},
        {GPIO_WRITE, 8, 0xaf},
        {CRG_WRITE, SET, GATE},
        {SPI_WRITE, IRQ, 0x10000000},
        {SPI_WRITE, ENABLE, 0x40},
        {SPI_WRITE, DIV, 0x155a0012},
        {SPI_WRITE, FORMAT, 0x800042c7},
        {SPI_WRITE, SELECT, 0xface0001},
        {CRG_WRITE, CTRL, 0x40200010},
    };
    static const struct event cold_cleanup[] = {
        {SPI_WRITE, ENABLE, 0x40},
        {GPIO_WRITE, 8, 0xa4},
        {CRG_WRITE, CLEAR, GATE},
    };
    struct sd5113_spi ss = reset(false);
    const u8 tx = 0x73;

    assert(!sd_spi_start(&ss, 18));
    expect_events(start, ARRAY_SIZE(start));
    assert(hw.crg.status == (0x08U | GATE) && hw.pads.funcs == 0xaf && ss.divisor == 18);
    hw.count = 0;
    cleanup(&ss, 0x40200010, 0x00600100);
    expect_events(cold_cleanup, ARRAY_SIZE(cold_cleanup));
    assert(hw.crg.status == 0x08U);

    ss = reset(true);
    assert(!sd_spi_start(&ss, 18));
    expect_events(start, ARRAY_SIZE(start));
    hw.status_tail = 0x6;
    assert(!sd_spi_byte(&ss, &tx, NULL));
    cleanup(&ss, 0x40200010, 0x00600100);
    assert_spi_snapshot();

    ss = reset(true);
    hw.crg.ctrl &= ~MUX;
    hw.crg.route &= ~MUX;
    hw.drop_select_write = true;
    assert(sd_spi_start(&ss, 18) == -EIO);
    assert((hw.crg.ctrl & MUX) && (hw.crg.route & MUX));
    cleanup(&ss, 0x40000010, 0x00400100);
    assert_spi_snapshot();

    for (unsigned i = 0; i < 3; ++i) {
        ss = reset(true);
        if (i == 0)
            hw.spi[ENABLE / 4] |= 1;
        else
            hw.status_tail = i == 1 ? 1 : 8;
        assert(sd_spi_start(&ss, 18) == -EBUSY);
        assert(hw.count == 0 && !hw.dev.cleanup);
    }
    ss = reset(false);
    hw.pads.funcs |= 0x10;
    assert(sd_spi_start(&ss, 18) == -EBUSY);
    assert(hw.count == 0 && !hw.dev.cleanup && hw.pads.funcs == 0xb4);
    assert(sd_spi_funcs(&ss, 0x10, 0) == -EINVAL);
    ss = reset(true);
    assert(sd_spi_start(&ss, 18) == 0);
    hw.pads.funcs |= 0x10; /* A later external mux change must survive unbind. */
    hw.count = 0;
    hw.dev.cleanup(hw.dev.cleanup_data);
    assert(hw.pads.funcs == 0xb4);
    assert_spi_snapshot();
}

static void test_group(void)
{
    struct sd5113_spi ss = reset(true);
    struct spi_controller ctlr = {.priv = &ss};
    struct spi_message msg;
    struct spi_transfer xfer[2] = {
        {.len = 1, .speed_hz = 8000000, .tx_buf = "A", .cs_change = true},
        {.len = 1, .speed_hz = 8000000, .tx_buf = "B"},
    };
    struct list_head *head = &msg.transfers;
    static const struct event reselect[] = {
        {CRG_WRITE, CTRL, 0x40000010},
        {GPIO_WRITE, 8, 0xaf},
        {SPI_WRITE, ENABLE, 0x40},
        {SPI_WRITE, DIV, 0x155a0012},
        {SPI_WRITE, FORMAT, 0x800042c7},
        {SPI_WRITE, SELECT, 0xface0001},
        {CRG_WRITE, CTRL, 0x40200010},
    };

    ss.ahb = 133250000;
    assert(!sd_spi_start(&ss, 18));
    hw.count = 0;
    hw.pads.funcs = 0xa7;
    *head = (struct list_head){ &xfer[0].transfer_list, &xfer[1].transfer_list };
    xfer[0].transfer_list = (struct list_head){&xfer[1].transfer_list, head};
    xfer[1].transfer_list = (struct list_head){head, &xfer[0].transfer_list};

    assert(sd_spi_prepare_message(&ctlr, &msg) == 0);
    expect_events(reselect, ARRAY_SIZE(reselect));
    hw.count = 0;
    xfer[0].cs_change = false;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EOPNOTSUPP && hw.count == 0);
    xfer[0].cs_change = true;
    xfer[1].speed_hz = 7000000;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EOPNOTSUPP && hw.count == 0);
    xfer[1].speed_hz = 8000000;
    hw.status_tail = 1;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EBUSY && hw.count == 0);
    hw.status_tail = 6;
    hw.pads.funcs |= 0x10;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EBUSY && hw.count == 0);
    assert(hw.pads.funcs == 0xbf);
    hw.pads.funcs &= ~0x10U;
    hw.crg.route &= ~MUX;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EIO);
    hw.crg.route |= MUX;
    hw.pads.funcs &= ~1U;
    assert(sd_spi_prepare_message(&ctlr, &msg) == -EIO);
}

int main(void)
{
    test_divisor();
    puts("PASS: divisor ceil-even, limits, and no overspeed");
    test_bytes();
    puts("PASS: TX/RX FIFO and status sequencing, invalid directions, RUN cleared on timeouts");
    test_start_restore();
    puts("PASS: cold start order, cold/warm restore, failed readback rollback, busy rejection");
    test_group();
    puts("PASS: CS0-only writes, GPIO4 guard and grouped CS/speed/route checks");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc', help='GNU C-compatible host compiler')
    parser.add_argument('--source', type=Path, default=SOURCE)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    helpers = source[source.index('#define SD_SPI_FORMAT'):
                      source.index('static int sd_spi_setup(')]
    helpers += source[source.index('static int sd_spi_idle('):
                      source.index('static int sd_spi_transfer_one(')]
    with tempfile.TemporaryDirectory(prefix='sd5113-spi-host-test-') as directory:
        c_file = Path(directory) / 'test.c'
        executable = Path(directory) / 'test'
        c_file.write_text(PRELUDE + '\n' + helpers + TESTS, encoding='utf-8')
        subprocess.run([args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(c_file), '-o', str(executable)], cwd=directory, check=True)
        subprocess.run([str(executable)], cwd=directory, check=True, timeout=10)


if __name__ == '__main__':
    main()

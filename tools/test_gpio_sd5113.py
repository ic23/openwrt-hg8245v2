"""Compile the active SD5113 GPIO register paths against a fake MMIO bank map."""
from pathlib import Path
import subprocess
import tempfile

SOURCE = (Path(__file__).resolve().parents[1] /
          "openwrt-target/target/linux/sd5113/files-6.6/drivers/gpio/gpio-sd5113.c")
MOCK = r"""
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define GPIO_LINE_DIRECTION_IN 1
#define GPIO_LINE_DIRECTION_OUT 0
#define EXPORT_SYMBOL_GPL(n)
struct regmap { u32 mux; };
struct gpio_chip {
    int base;
    int (*request)(struct gpio_chip *, unsigned int);
};
struct gpio_desc { struct gpio_chip *chip; int pin; };
typedef int spinlock_t;
static unsigned held;
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 1, assert(!held++))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), assert((flags) && held-- == 1))
static u32 regs[8 * 0x1000 / sizeof(u32)];
static struct { ptrdiff_t offset; u32 value; } writes[8];
static unsigned count;
static u32 readl(u32 *p) { return *p; }
static void writel(u32 n, u32 *p)
{
    assert(held == 1 && count < ARRAY_SIZE(writes));
    writes[count].offset = (char *)p - (char *)regs;
    writes[count++].value = *p = n;
}
static int regmap_set_bits(struct regmap *m, unsigned int off, u32 mask)
{
    assert(off == 0x54);
    m->mux |= mask;
    return 0;
}
static struct gpio_chip *gpiod_to_chip(struct gpio_desc *d) { return d->chip; }
static int desc_to_gpio(struct gpio_desc *d) { return d->chip->base + d->pin; }
"""

TEST = r"""
int main(void)
{
    struct regmap crg = {0x80};
    struct sd5113_gpio gpio = {
        .chip = {512, sd5113_gpio_request},
        .base = regs, .crg = &crg,
    };
    struct gpio_chip *c = &gpio.chip;
    struct gpio_chip other = {128, sd5113_gpio_request};
    struct gpio_desc a = {c, 18}, b = {c, 21}, wrong_bank = {c, 10}, wrong_chip = {&other, 18};
    struct gpio_desc scl_sda[] = {{c, 5}, {c, 6}};
    struct gpio_desc *i2c_pads[] = {&scl_sda[0], &scl_sda[1]};
    struct gpio_desc cs0[] = {{c, 0}, {c, 1}, {c, 2}, {c, 3}};
    struct gpio_desc *spi_pads[] = {&cs0[0], &cs0[1], &cs0[2], &cs0[3]};
    struct sd5113_gpio_bank_state state;
    struct gpio_desc *pads[] = {&a, &b};
    u32 *bank = regs + 0x2000 / 4;
    u32 old;

    assert(!sd5113_gpio_request(c, 4) && crg.mux == 0x80);
    assert(!sd5113_gpio_request(c, 44) && crg.mux == 0x81);
    bank[0] = 0x14;
    bank[1] = 0xa0;
    bank[2] = 0x85;
    bank[0x50 / 4] = 0x20;
    assert(sd5113_gpio_get(c, 21) == 1 && sd5113_gpio_get(c, 18) == 0);
    assert(sd5113_gpio_get_direction(c, 18) == GPIO_LINE_DIRECTION_IN);

    assert(!sd5113_gpio_direction_output(c, 18, 0));
    assert(count == 3 && writes[0].offset == 0x2000 && writes[0].value == 0x10);
    assert(writes[1].offset == 0x2004 && writes[1].value == 0xa4);
    assert(writes[2].offset == 0x2008 && writes[2].value == 0x81);
    assert(sd5113_gpio_get_direction(c, 18) == GPIO_LINE_DIRECTION_OUT);
    count = 0;
    sd5113_gpio_set(c, 18, 1);
    assert(count == 1 && writes[0].offset == 0x2000 && writes[0].value == 0x14);
    count = 0;
    bank[2] |= BIT(2);
    assert(!sd5113_gpio_direction_input(c, 18));
    assert(count == 2 && writes[0].offset == 0x2004 && writes[0].value == 0xa0);
    assert(writes[1].offset == 0x2008 && writes[1].value == 0x81 && bank[0] == 0x14);
    assert(sd5113_gpio_get_direction(c, 18) == GPIO_LINE_DIRECTION_IN);
    count = 0;
    assert(!sd5113_gpio_set_funcs(pads, 2, 0, 0, &old) && old == 0 && !count);
    assert(!sd5113_gpio_set_funcs(pads, 2, 0x24, 0x24, &old) && old == 0);
    assert(count == 1 && writes[0].offset == 0x2008 && writes[0].value == 0xa5);
    assert(bank[1] == 0xa0 && bank[0] == 0x14 && regs[2] == 0);
    assert(!sd5113_gpio_set_funcs(pads, 2, 0x24, old, NULL) && bank[2] == 0x81);
    assert(sd5113_gpio_set_funcs(pads, 2, 0x40, 0x40, NULL) == -EINVAL);
    pads[1] = &a;
    assert(sd5113_gpio_set_funcs(pads, 2, 0, 0, NULL) == -EINVAL);
    pads[1] = &wrong_bank;
    assert(sd5113_gpio_set_funcs(pads, 2, 0, 0, NULL) == -EINVAL);
    pads[1] = &wrong_chip;
    assert(sd5113_gpio_set_funcs(pads, 2, 0, 0, NULL) == -EINVAL);
    assert(count == 2 && !held);
    count = 0;
    regs[0] = regs[1] = 0x10; /* GPIO4 high and output in the panel lab. */
    regs[2] = 0x80;
    assert(!sd5113_gpio_set_funcs(i2c_pads, 2, 0x60, 0x60, &old) && old == 0);
    assert(count == 1 && writes[0].offset == 8 && writes[0].value == 0xe0);
    assert(regs[0] == 0x10 && regs[1] == 0x10 && bank[2] == 0x81);
    assert(!sd5113_gpio_read_bank_funcs(&scl_sda[0], &old) && old == 0xe0);
    assert(!sd5113_gpio_read_bank_funcs(&a, &old) && old == 0x81);
    assert(sd5113_gpio_read_bank_funcs(&a, NULL) == -EINVAL);
    assert(count == 1);
    count = 0;
    assert(!sd5113_gpio_direction_output(c, 4, 1));
    assert(!sd5113_gpio_set_funcs(spi_pads, 4, 0x0f, 0x0f, &old) && old == 0);
    assert(regs[2] == 0xef && regs[1] == 0x10 && regs[0] == 0x10);
    sd5113_gpio_set(c, 4, 0);
    assert(regs[2] == 0xef && regs[1] == 0x10 && regs[0] == 0);
    assert(!sd5113_gpio_set_funcs(spi_pads, 4, 0x0f, old, NULL));
    assert(regs[2] == 0xe0 && regs[1] == 0x10 && regs[0] == 0);
    puts("PASS: GPIO4 LED toggles with CS0 and I2C FUNC active; ownership and rollback preserved");

    count = 0;
    bank = regs + 0x4000 / 4;
    bank[0] = 0xe3;
    bank[1] = 0xe3;
    bank[2] = 0;
    bank[0x50 / 4] = 0xff;
    sd5113_gpio_read_bank(&gpio, 4, &state);
    assert(state.data == 0xe3 && state.dir == 0xe3 && state.func == 0 && state.level == 0xff);
    bank = regs + 0x5000 / 4;
    bank[0] = 0x8b;
    bank[1] = 0xcb;
    bank[2] = 4;
    bank[0x50 / 4] = 0x8f;
    sd5113_gpio_read_bank(&gpio, 5, &state);
    assert(state.data == 0x8b && state.dir == 0xcb && state.func == 4 && state.level == 0x8f);
    assert(count == 0 && !held);
    puts("PASS: read-only GPIO bank snapshots");
}
"""

if __name__ == "__main__":
    source = SOURCE.read_text(encoding="utf-8")
    helpers = source[source.index("#define SD5113_GPIO_NPINS"):
                     source.index("static int sd5113_gpio_banks_show(")]
    with tempfile.TemporaryDirectory(prefix="sd5113-gpio-") as path:
        d = Path(path)
        (d / "test.c").write_text(MOCK + helpers + TEST, encoding="utf-8")
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "test.c", "-o", "test"], cwd=d, check=True)
        subprocess.run([str(d / "test")], cwd=d, check=True, timeout=5)

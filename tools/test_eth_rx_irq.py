"""Compile the actual SD5113 RX-latch helper against scripted MMIO."""

import argparse
import re

from test_mdio_transactions import ROOT, run_host_test

SOURCE = (ROOT / "openwrt-target/target/linux/sd5113/files-6.6"
          / "drivers/net/ethernet/hisilicon/eth-sd5113.c")

PRELUDE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
struct sd5113_eth { unsigned char *cpurtx, *glb; };
static unsigned char rtx[0x34], global[8];
static u32 slots, latch;
static unsigned int writes;
static bool race;

static u32 readl(const void *addr)
{
    if (addr == rtx + 0x30)
        return slots;
    assert(addr == global);
    return latch;
}

static void writel(u32 value, void *addr)
{
    assert(addr == global);
    assert(value == BIT(4));
    ++writes;
    if (race)
        slots |= BIT(6);
    latch &= ~value;
}
"""

TESTS = r"""
int main(void)
{
    struct sd5113_eth priv = { rtx, global };

    latch = BIT(4) | BIT(8);
    slots = BIT(7);
    assert(!sd5113_eth_finish_rx_irq(&priv));
    assert(writes == 0 && latch == (BIT(4) | BIT(8)));

    slots = BIT(9); /* A non-RX cause does not prevent clearing the RX latch. */
    assert(sd5113_eth_finish_rx_irq(&priv));
    assert(writes == 1 && latch == BIT(8));
    assert(sd5113_eth_finish_rx_irq(&priv));
    assert(writes == 1);

    latch |= BIT(4);
    slots = 0;
    race = true;
    assert(!sd5113_eth_finish_rx_irq(&priv));
    assert(writes == 2 && slots == BIT(6) && latch == BIT(8));

    puts("PASS: RX latch drains only when slots are empty, preserves unrelated IRQ bits, and catches a new frame during W1C");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = SOURCE.read_text(encoding="utf-8")
    definitions = "\n".join(re.findall(
        r"^#define (?:GLB_IRQ_CAUSE|GLB_ETH_IRQ_BIT|CPURTX_CAUSE|SD5113_RX_NUM_SLOTS|SD5113_RX_SLOT_MASK)\b[^\n]*",
        source, re.MULTILINE))
    # CPURTX_CAUSE has a multi-line comment; collect the value without it.
    definitions = re.sub(r"[ \t]*/\*[^\n]*", "", definitions)
    helper = source[source.index("static bool sd5113_eth_finish_rx_irq"):
                    source.index("static int sd5113_eth_poll")]
    run_host_test(args.cc, "\n".join([PRELUDE, definitions, helper, TESTS]))


if __name__ == "__main__":
    main()

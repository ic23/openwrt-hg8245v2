"""Host tests for the SD5113 CPU-path VLAN and TX descriptor helpers."""

import argparse
import re

from test_mdio_transactions import ROOT, run_host_test

SOURCE = (ROOT / "openwrt-target/target/linux/sd5113/files-6.6"
          / "drivers/net/ethernet/hisilicon/eth-sd5113.c")

PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t dma_addr_t;
#define BIT(n) (1U << (n))
#define ETH_ALEN 6
#define ETH_ZLEN 60
#define ETH_P_8021Q 0x8100
#define VLAN_VID_MASK 0x0fff
#define VLAN_ETH_HLEN 18
#define VLAN_HLEN 4
struct sk_buff { u8 *data; unsigned int len; };
struct sd5113_fe { void *broadcast; };
struct sd5113_eth { u8 *cpurtx; struct sd5113_fe *fe; void *ndev; };
static u8 mmio[16];
static unsigned int writes;
static struct { unsigned int off; u32 value; } log_writes[4];

static unsigned int get_unaligned_be16(const u8 *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

static void skb_pull(struct sk_buff *skb, unsigned int len)
{
    assert(len <= skb->len);
    skb->data += len;
    skb->len -= len;
}

static u32 readl(const void *p)
{
    assert(p == mmio);
    return 0x10;
}

static void writel(u32 value, void *p)
{
    assert(p >= (void *)mmio && p < (void *)(mmio + sizeof(mmio)));
    assert(writes < 4);
    log_writes[writes].off = (unsigned int)((u8 *)p - mmio);
    log_writes[writes++].value = value;
}
"""

TESTS = r"""
int main(void)
{
    u8 frame[64] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                    0x02, 1, 2, 3, 4, 5, 0x81, 0, 0, 1, 0x08, 0x06};
    u8 input[64];
    struct sk_buff skb = { input, 64 };
    struct sd5113_fe fe = { mmio };
    struct sd5113_eth priv = { mmio, &fe, mmio };

    memcpy(input, frame, 64);
    sd5113_eth_pull_cpu_tag(&skb);
    assert(skb.len == 60 && skb.data == input + 4);
    assert(!memcmp(skb.data, frame, 12));
    assert(!memcmp(skb.data + 12, frame + 16, 48));

    memcpy(input, frame, 64);
    input[14] = 0xa0; /* PCP is not part of the VID. */
    input[16] = 0x81; input[17] = 0; input[19] = 0x2a;
    skb = (struct sk_buff){ input, 64 };
    sd5113_eth_pull_cpu_tag(&skb);
    assert(skb.len == 60 && get_unaligned_be16(skb.data + 12) == 0x8100);
    assert(get_unaligned_be16(skb.data + 14) == 0x002a);

    memcpy(input, frame, 64);
    input[15] = 2;
    skb = (struct sk_buff){ input, 64 };
    sd5113_eth_pull_cpu_tag(&skb);
    assert(skb.len == 64 && skb.data == input && input[15] == 2);
    input[15] = 1;
    skb.len = 17;
    sd5113_eth_pull_cpu_tag(&skb);
    assert(skb.len == 17 && skb.data == input);
    skb.len = 64;
    input[12] = 0x08; input[13] = 0x06;
    sd5113_eth_pull_cpu_tag(&skb);
    assert(skb.len == 64 && skb.data == input);

    sd5113_eth_program_tx(&priv, 0x92340002, 60, 0);
    assert(writes == 4);
    assert(log_writes[0].off == 4 && log_writes[0].value == 60U << 4);
    assert(log_writes[1].off == 12 && log_writes[1].value == 4);
    assert(log_writes[2].off == 8 && log_writes[2].value == 0x92340002);
    assert(log_writes[3].off == 0 && log_writes[3].value == 0x11);

    writes = 0;
    sd5113_eth_program_tx(&priv, 0x92340002, 302, BIT(31) | (302U << 19) | (3U << 12) | 1020);
    assert(writes == 4 && log_writes[0].value == ((302U << 4) | 1));
    assert(log_writes[1].off == 12 && log_writes[1].value == ((1020U << 10) | (3U << 6) | BIT(5)));
    assert(log_writes[2].value == 0x92340002 && log_writes[3].value == 0x11);
    writes = 0;
    priv.fe = NULL;
    sd5113_eth_program_tx(&priv, 0x92340002, 60, 0);
    assert(writes == 3 && log_writes[0].off == 4);
    assert(log_writes[0].value == ((60U << 4) | (60U << 21)));
    assert(log_writes[1].off == 8 && log_writes[2].off == 0);
    priv.fe = &fe;
    memcpy(input, frame, sizeof(frame));
    skb = (struct sk_buff){ input, 46 };
    assert(padded_len(&priv, &skb, priv.ndev) == 64);
    assert(padded_len(&priv, &skb, NULL) == 60);
    skb.len = 102;
    assert(padded_len(&priv, &skb, priv.ndev) == 102);
    skb.len = 46;
    fe.broadcast = NULL;
    assert(padded_len(&priv, &skb, priv.ndev) == 60);
    fe.broadcast = mmio;
    input[12] = 8; input[13] = 6;
    assert(padded_len(&priv, &skb, priv.ndev) == 60);
    puts("PASS: CPU tag removal, LAN TX descriptor and frame padding after trunk tag removal");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = SOURCE.read_text(encoding="utf-8")
    definitions = "\n".join(re.findall(
        r"^#define (?:CPURTX_TX_(?:CTRL[01]|BUFADDR|KICK|PENDING)|SD5113_FE_(?:CPU_VID|TX_CTRL1))\b[^\n]*",
        source, re.MULTILINE))
    definitions = re.sub(r"[ \t]*/\*[^\n]*", "", definitions)
    tx = source[source.index("static void sd5113_eth_program_tx"):
                source.index("\n}", source.index("static void sd5113_eth_program_tx")) + 2]
    rx = source[source.index("static void sd5113_eth_pull_cpu_tag"):
                source.index("static void sd5113_eth_rx_slot")]
    min_init = re.search(r"unsigned int min_len = [^;]+;", source).group()
    tx_min = source[source.index("if (ndev == priv->ndev && priv->fe && priv->fe->broadcast"):
                    source.index("if (skb_put_padto(skb, min_len))")]
    pad = ("static unsigned int padded_len(struct sd5113_eth *priv, struct sk_buff *skb, "
           "void *ndev) { " + min_init + tx_min +
           "return skb->len < min_len ? min_len : skb->len; }")
    run_host_test(args.cc, "\n".join([PRELUDE, definitions, tx, rx, pad, TESTS]))


if __name__ == "__main__":
    main()

/* SPDX-License-Identifier: GPL-2.0-only */
/* Sizes and access: omci-lib-go v2.2.4 (generated, Apache-2.0).
  * (Apache-2.0, Boling Consulting / ONF). The Go is not copied;
  * only size, type and access. 148/157 follow G.988 on set-by-create.
  * 297, 347 and 65530 are not in omci-lib-go: G.988 9.1.8, 9.4.5 and CTC LOID.
  * F_NOUP copies the E/U letter of the 2026-09-28 meg. */
#include "omci.h"

static const struct omci_attr A2[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A2) / sizeof(A2[0]) == 1, "A2");

static const struct omci_attr A5[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 20, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 20, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A5) / sizeof(A5[0]) == 9, "A5");

static const struct omci_attr A6[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 8, OMCI_KIND_OCTETS, OMCI_F_R },
    { 14, OMCI_KIND_OCTETS, OMCI_F_R },
    { 4, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 20, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A6) / sizeof(A6[0]) == 14, "A6");

static const struct omci_attr A7[] = {
    { 14, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R },
};
_Static_assert(sizeof(A7) / sizeof(A7[0]) == 6, "A7");

static const struct omci_attr A11[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
};
_Static_assert(sizeof(A11) / sizeof(A11[0]) == 15, "A11");

static const struct omci_attr A45[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A45) / sizeof(A45[0]) == 10, "A45");

static const struct omci_attr A47[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 6, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A47) / sizeof(A47[0]) == 14, "A47");

static const struct omci_attr A84[] = {
    { 24, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A84) / sizeof(A84[0]) == 3, "A84");

static const struct omci_attr A130[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 24, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A130) / sizeof(A130[0]) == 13, "A130");

static const struct omci_attr A131[] = {
    { 4, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 20, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 14, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 14, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A131) / sizeof(A131[0]) == 4, "A131");

static const struct omci_attr A133[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
};
_Static_assert(sizeof(A133) / sizeof(A133[0]) == 11, "A133");

static const struct omci_attr A134[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 6, OMCI_KIND_OCTETS, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 4, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_NOUP },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A134) / sizeof(A134[0]) == 16, "A134");

static const struct omci_attr A137[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A137) / sizeof(A137[0]) == 2, "A137");

static const struct omci_attr A148[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A148) / sizeof(A148[0]) == 5, "A148");

static const struct omci_attr A157[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A157) / sizeof(A157[0]) == 16, "A157");

static const struct omci_attr A158[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_W },
    { 1, OMCI_KIND_TABLE, OMCI_F_R },
};
_Static_assert(sizeof(A158) / sizeof(A158[0]) == 3, "A158");

static const struct omci_attr A160[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A160) / sizeof(A160[0]) == 2, "A160");

static const struct omci_attr A171[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_TABLE, OMCI_F_R | OMCI_F_W | OMCI_F_NOUP },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 24, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C },
    { 28, OMCI_KIND_TABLE, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A171) / sizeof(A171[0]) == 10, "A171");

static const struct omci_attr A256[] = {
    { 4, OMCI_KIND_OCTETS, OMCI_F_R },
    { 14, OMCI_KIND_OCTETS, OMCI_F_R },
    { 8, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 24, OMCI_KIND_OCTETS, OMCI_F_R },
    { 12, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
};
_Static_assert(sizeof(A256) / sizeof(A256[0]) == 13, "A256");

static const struct omci_attr A257[] = {
    { 20, OMCI_KIND_OCTETS, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A257) / sizeof(A257[0]) == 14, "A257");

static const struct omci_attr A262[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A262) / sizeof(A262[0]) == 3, "A262");

static const struct omci_attr A263[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A263) / sizeof(A263[0]) == 16, "A263");

static const struct omci_attr A264[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A264) / sizeof(A264[0]) == 5, "A264");

static const struct omci_attr A266[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A266) / sizeof(A266[0]) == 8, "A266");

static const struct omci_attr A268[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W | OMCI_F_NOUP },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP | OMCI_F_NS },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_NOUP | OMCI_F_NS },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_C | OMCI_F_W },
};
_Static_assert(sizeof(A268) / sizeof(A268[0]) == 10, "A268");

static const struct omci_attr A277[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 8, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A277) / sizeof(A277[0]) == 16, "A277");

static const struct omci_attr A278[] = {
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A278) / sizeof(A278[0]) == 4, "A278");

static const struct omci_attr A297[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A297) / sizeof(A297[0]) == 9, "A297");

static const struct omci_attr A329[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
};

static const struct omci_attr A336[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 2, OMCI_KIND_INT, OMCI_F_R },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 2, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 8, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 4, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
};
_Static_assert(sizeof(A336) / sizeof(A336[0]) == 12, "A336");

static const struct omci_attr A347[] = {
    { 1, OMCI_KIND_INT, OMCI_F_R | OMCI_F_W },
    { 6, OMCI_KIND_OCTETS, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 16, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 24, OMCI_KIND_TABLE, OMCI_F_R },
    { 16, OMCI_KIND_TABLE, OMCI_F_R },
    { 16, OMCI_KIND_TABLE, OMCI_F_R },
    { 25, OMCI_KIND_OCTETS, OMCI_F_R },
    { 17, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 26, OMCI_KIND_TABLE, OMCI_F_R },
};
_Static_assert(sizeof(A347) / sizeof(A347[0]) == 14, "A347");

static const struct omci_attr A65530[] = {
    { 4, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 24, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 12, OMCI_KIND_OCTETS, OMCI_F_R | OMCI_F_W },
    { 1, OMCI_KIND_INT, OMCI_F_R },
};
_Static_assert(sizeof(A65530) / sizeof(A65530[0]) == 4, "A65530");

static const struct omci_class CLASSES[] = {
    { 2, "ONU data", A2, (uint8_t)(sizeof(A2) / sizeof(A2[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 5, "Cardholder", A5, (uint8_t)(sizeof(A5) / sizeof(A5[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 6, "Circuit pack", A6, (uint8_t)(sizeof(A6) / sizeof(A6[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 7, "Software image", A7, (uint8_t)(sizeof(A7) / sizeof(A7[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 11, "PPTP Ethernet UNI", A11, (uint8_t)(sizeof(A11) / sizeof(A11[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 45, "MAC bridge service profile", A45, (uint8_t)(sizeof(A45) / sizeof(A45[0])), 1, 1, 0, OMCI_APPLY_BRIDGE },
    { 47, "MAC bridge port configuration data", A47, (uint8_t)(sizeof(A47) / sizeof(A47[0])), 1, 1, 0, OMCI_APPLY_BRIDGE_PORT },
    { 84, "VLAN tagging filter data", A84, (uint8_t)(sizeof(A84) / sizeof(A84[0])), 1, 1, 0, OMCI_APPLY_VLAN_FILTER },
    { 130, "802.1p mapper service profile", A130, (uint8_t)(sizeof(A130) / sizeof(A130[0])), 1, 1, 0, OMCI_APPLY_PMAPPER },
    { 131, "OLT-G", A131, (uint8_t)(sizeof(A131) / sizeof(A131[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 133, "ONU power shedding", A133, (uint8_t)(sizeof(A133) / sizeof(A133[0])), 0, 0, 0, OMCI_APPLY_LASER },
    { 134, "IP host config data", A134, (uint8_t)(sizeof(A134) / sizeof(A134[0])), 0, 0, 1, OMCI_APPLY_STORE },
    { 137, "Network address", A137, (uint8_t)(sizeof(A137) / sizeof(A137[0])), 1, 1, 1, OMCI_APPLY_STORE },
    { 148, "Authentication security method", A148, (uint8_t)(sizeof(A148) / sizeof(A148[0])), 1, 1, 1, OMCI_APPLY_STORE },
    { 157, "Large string", A157, (uint8_t)(sizeof(A157) / sizeof(A157[0])), 1, 1, 1, OMCI_APPLY_STORE },
    { 158, "ONU remote debug", A158, (uint8_t)(sizeof(A158) / sizeof(A158[0])), 0, 0, 0, OMCI_APPLY_DEBUG },
    { 160, "Equipment extension package", A160, (uint8_t)(sizeof(A160) / sizeof(A160[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 171, "Extended VLAN tagging operation configuration data", A171, (uint8_t)(sizeof(A171) / sizeof(A171[0])), 1, 1, 0, OMCI_APPLY_EXT_VLAN },
    { 256, "ONU-G", A256, (uint8_t)(sizeof(A256) / sizeof(A256[0])), 0, 0, 0, OMCI_APPLY_ONU_G },
    { 257, "ONU2-G", A257, (uint8_t)(sizeof(A257) / sizeof(A257[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 262, "T-CONT", A262, (uint8_t)(sizeof(A262) / sizeof(A262[0])), 0, 0, 0, OMCI_APPLY_TCONT },
    { 263, "ANI-G", A263, (uint8_t)(sizeof(A263) / sizeof(A263[0])), 0, 0, 0, OMCI_APPLY_ANI },
    { 264, "UNI-G", A264, (uint8_t)(sizeof(A264) / sizeof(A264[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 266, "GEM interworking termination point", A266, (uint8_t)(sizeof(A266) / sizeof(A266[0])), 1, 1, 0, OMCI_APPLY_GEM_IW },
    { 268, "GEM port network CTP", A268, (uint8_t)(sizeof(A268) / sizeof(A268[0])), 1, 1, 0, OMCI_APPLY_GEM_PORT },
    { 277, "Priority queue", A277, (uint8_t)(sizeof(A277) / sizeof(A277[0])), 1, 1, 0, OMCI_APPLY_QUEUE },
    { 278, "Traffic scheduler", A278, (uint8_t)(sizeof(A278) / sizeof(A278[0])), 1, 1, 0, OMCI_APPLY_QUEUE },
    { 297, "Port-mapping package", A297, (uint8_t)(sizeof(A297) / sizeof(A297[0])), 0, 0, 0, OMCI_APPLY_NONE },
    { 329, "Virtual Ethernet interface point", A329, 5, 0, 0, 0, OMCI_APPLY_NONE },
    { 336, "ONU dynamic power management control", A336, (uint8_t)(sizeof(A336) / sizeof(A336[0])), 0, 0, 0, OMCI_APPLY_LASER },
    { 347, "IPv6 host config data", A347, (uint8_t)(sizeof(A347) / sizeof(A347[0])), 0, 0, 1, OMCI_APPLY_STORE },
    { 65530, "LOID authentication", A65530, (uint8_t)(sizeof(A65530) / sizeof(A65530[0])), 0, 0, 1, OMCI_APPLY_STORE },
};

_Static_assert(sizeof(CLASSES) / sizeof(CLASSES[0]) == 32, "32 classes");

const struct omci_class *omci_class_by_id(uint16_t id)
{
    size_t i;
    for (i = 0; i < sizeof(CLASSES) / sizeof(CLASSES[0]); i++) {
        if (CLASSES[i].id == id)
            return &CLASSES[i];
    }
    return NULL;
}

const struct omci_class *omci_classes(size_t *n)
{
    if (n)
        *n = sizeof(CLASSES) / sizeof(CLASSES[0]);
    return CLASSES;
}

int omci_attr_readable(const struct omci_attr *a)
{
    return a && (a->flags & OMCI_F_R) != 0;
}

int omci_attr_writable(const struct omci_attr *a)
{
    return a && (a->flags & OMCI_F_W) != 0 && (a->flags & OMCI_F_NS) == 0;
}

int omci_attr_set_by_create(const struct omci_attr *a)
{
    return a && (a->flags & OMCI_F_C) != 0;
}

int omci_attr_uploadable(const struct omci_attr *a)
{
    return a && a->kind != OMCI_KIND_TABLE && omci_attr_readable(a) &&
           (a->flags & OMCI_F_NOUP) == 0;
}

int omci_classes_consistent(void)
{
    size_t n = 0;
    size_t i, a;
    const struct omci_class *cs = omci_classes(&n);
    static const uint16_t expect[] = {
        2, 5, 6, 7, 11, 45, 47, 84, 130, 131, 133, 134, 137, 148, 157, 158,
        160, 171, 256, 257, 262, 263, 264, 266, 268, 277, 278, 297, 329, 336, 347,
        65530
    };

    if (n != sizeof(expect) / sizeof(expect[0]))
        return 0;
    for (i = 0; i < n; i++) {
        if (cs[i].id != expect[i])
            return 0;
        if (cs[i].n_attrs == 0 || cs[i].n_attrs > OMCI_MAX_ATTR)
            return 0;
        for (a = 0; a < cs[i].n_attrs; a++) {
            if (cs[i].attrs[a].size == 0 || cs[i].attrs[a].size > OMCI_VALUE_MAX)
                return 0;
            if (omci_attr_uploadable(&cs[i].attrs[a]) &&
                cs[i].attrs[a].size > OMCI_UPLOAD_NEXT_BUDGET)
                return 0;
        }
    }
    return 1;
}


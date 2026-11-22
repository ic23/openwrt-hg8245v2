#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OVERLAY="$ROOT/openwrt-target/target/linux/sd5113/files-6.6"
DTS=${1:-"$OVERLAY/arch/arm/boot/dts/hisilicon/hisilicon-sd5113-hg8245.dts"}
: "${KERNEL_COMMON_INCLUDE:?set KERNEL_COMMON_INCLUDE to the Linux 6.6 include directory}"
OUT=${2:-hisilicon-sd5113-hg8245.dtb}
PREPROCESSED=$(mktemp)
trap 'rm -f "$PREPROCESSED"' EXIT
"${CC:-cc}" -E -nostdinc -undef -P -x assembler-with-cpp \
  -I "$OVERLAY/include" -I "$KERNEL_COMMON_INCLUDE" "$DTS" -o "$PREPROCESSED"
"${DTC:-dtc}" -I dts -O dtb -o "$OUT" "$PREPROCESSED"

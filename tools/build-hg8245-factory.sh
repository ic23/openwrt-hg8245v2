#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
if (( $# > 1 )); then
	echo "Usage: $0 [stock-nor.bin]" >&2
	exit 2
fi
stock_args=()
if [[ -n ${1:-${HG8245_STOCK_DUMP:-}} ]]; then
	HG8245_STOCK_DUMP=$(realpath -e -- "${1:-$HG8245_STOCK_DUMP}")
	export HG8245_STOCK_DUMP
	stock_args+=("$HG8245_STOCK_DUMP")
	python3 "$project/tools/hg8245_factory.py" "$HG8245_STOCK_DUMP"
fi
release=rc12
export HG8245_CONFIG="$project/openwrt-target/config.sd5113_hg8245.rc12"
work=${HG8245_BUILD_HOME:-"$HOME/.cache/hg8245-build"}
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
export HG8245_ARTIFACT_DIR="$tmp/recovery-packages"
export SOURCE_DATE_EPOCH=1784877710
bash "$project/tools/build-hg8245-ram.sh"
src="$work/openwrt-v24.10.8-hg8245"
cd "$src"
linuxdirs=(build_dir/target-arm_*/linux-sd5113_generic)
compilers=(staging_dir/toolchain-arm_*/bin/arm-openwrt-linux-muslgnueabi-gcc)
linux=${linuxdirs[0]}
cross=$PWD/${compilers[0]%-gcc}
root=${linux%/*}/root-sd5113
host=$PWD/staging_dir/host/bin
dts="$project/openwrt-target/target/linux/sd5113/files-6.6/arch/arm/boot/dts/hisilicon"
kheaders="$linux/linux-6.6.144"
for mode in recovery flash installer; do
	cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$kheaders/include" \
		"$dts/hisilicon-sd5113-hg8245-$mode.dts" |
		"$kheaders/scripts/dtc/dtc" -I dts -O dtb -o "$tmp/$mode.dtb"
done
make_image() {
	"$host/mkimage" -A arm -O linux -T "$1" -C none -a "$2" -e "$2" \
		-n "$3" -d "$4" "$5"
}
cat "$linux/zImage-initramfs" "$tmp/recovery.dtb" > "$tmp/ram"
image="$tmp/hg8245v2-$release-recovery.bin"
make_image kernel 0x91000000 "HG8245V2 $release recovery" "$tmp/ram" "$image"
python3 "$project/tools/verify_fxs_image.py" --gpon-qos-iptv --split-lan-forwarding \
	--pppoe --front-panel --panel-wifi --ethernet-panel --app luci-app-sd5113-gpon \
	--app luci-app-hg8245-telephony --app luci-app-ledtrig-hg8245 "$image"

cp "$linux/zImage" "$tmp/normal"
# Give bootm a word-aligned DTB and the entry shim an integral copy count.
truncate -s %4 "$tmp/normal"
dtb_offset=$(stat -c %s "$tmp/normal")
cat "$tmp/flash.dtb" >> "$tmp/normal"
truncate -s %4 "$tmp/normal"
payload_words=$(( $(stat -c %s "$tmp/normal") / 4 ))
"${cross}-gcc" -c -march=armv6 -marm -DDTB_OFFSET="$dtb_offset" -DPAYLOAD_WORDS="$payload_words" \
	"$project/tools/hg8245-flash-entry.S" -o "$tmp/entry.o"
"${cross}-objcopy" -O binary -j .text "$tmp/entry.o" "$tmp/entry"
make_image kernel 0x90008000 "HG8245V2 ${release} entry" "$tmp/entry" "$tmp/first"
# U-Boot compares ih_name with this literal when preparing the normal boot.
make_image ramdisk 0 squashfs "$tmp/normal" "$tmp/second"

test -x "$root/sbin/block"
for module in usb-storage sd_mod ext4 vfat; do
	test -s "$root/lib/modules/6.6.144/$module.ko"
done
test -x "$root/usr/bin/tcpdump"
test -x "$root/sbin/swconfig"
test -f "$root/lib/network/switch.sh"
test -s "$root/lib/modules/6.6.144/ar8xxx.ko"
test -x "$root/usr/sbin/sd5113-omcid"
test -x "$root/usr/sbin/hg8245-factory-backup"
test -f "$root/usr/share/luci/menu.d/luci-mod-status.json"
test -f "$root/usr/share/luci/menu.d/luci-mod-network.json"
"$host/mksquashfs4" "$root" "$tmp/root.squashfs" -all-root -no-xattrs -noappend -nopad \
	-comp xz -b 128k -p '/dev/console c 600 0 0 5 1' -e init lib/hg8245/ram-init
python3 "$project/tools/hg8245_factory.py" "${stock_args[@]}" \
	--pack "$tmp/first" "$tmp/second" "$tmp/root.squashfs" "$tmp/hg8245v2-${release}-factory-region.bin"
# Bind this installer's MTD writer to the compiled system region and to the
# supported bootloader and partition map without embedding per-unit identity.
region="$tmp/hg8245v2-${release}-factory-region.bin"
python3 "$project/tools/hg8245_sysupgrade.py" "$region" "$release" \
	"$tmp/hg8245v2-$release-sysupgrade.bin" "$host/fwtool"
python3 "$project/tools/hg8245_factory.py" "${stock_args[@]}" --bind-installer "$tmp/installer.dtb" "$region"
cat "$linux/zImage-initramfs" "$tmp/installer.dtb" > "$tmp/ram"
image="$tmp/hg8245v2-${release}-installer.bin"
make_image kernel 0x91000000 "HG8245V2 ${release} installer" "$tmp/ram" "$image"
PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_hg8245_installer.py" "$image" "$region" "${stock_args[@]}" \
	--sysupgrade "$tmp/hg8245v2-$release-sysupgrade.bin" --fwtool "$host/fwtool"
out=${HG8245_RELEASE_DIR:-"$project/artifacts/hg8245v2-${release}"}
mkdir -p "$out"
images=()
for mode in factory-region installer recovery sysupgrade; do
	images+=("hg8245v2-$release-$mode.bin")
done
(cd "$tmp" && sha256sum -- "${images[@]}" > SHA256SUMS)
for image in "${images[@]}"; do
	cp -- "$tmp/$image" "$out/$image"
done
# Earlier RC12 builds also published this diagnostic image in the release directory.
rm -f -- "$out/hg8245v2-$release-flat-lan.bin"
cp -- "$tmp/SHA256SUMS" "$out/SHA256SUMS"
cat "$out/SHA256SUMS"

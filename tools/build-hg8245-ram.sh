#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux || $EUID == 0 ]]; then
    echo "Run this build as a non-root user on Linux or WSL2." >&2
    exit 1
fi

project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
release=v24.10.8
revision=0b795ce79e23b553aa184080c390f9ce92a2b6d4
work=${HG8245_BUILD_HOME:-"$HOME/.cache/hg8245-build"}
origin=${HG8245_OPENWRT_GIT:-https://github.com/openwrt/openwrt.git}
jobs=${JOBS:-$(nproc)}
mkdir -p -- "$work"
work=$(cd -- "$work" && pwd)
case $(stat -f -c %T -- "$work") in
    9p|drvfs|fuseblk)
        echo "Use a native Linux filesystem for HG8245_BUILD_HOME, not /mnt/c." >&2
        exit 1
        ;;
esac
src="$work/openwrt-$release-hg8245"
if [[ ! -e "$src" ]]; then
    git clone --no-local --depth 1 --single-branch --branch "$release" "$origin" "$src"
fi
if [[ $(git -C "$src" rev-parse --verify HEAD) != "$revision" ]]; then
    echo "Unexpected OpenWrt revision in $src." >&2
    exit 1
fi

overlay="$project/openwrt-target"
target_config=$(realpath -e -- "${HG8245_CONFIG:-"$overlay/config.sd5113_hg8245.rc12"}")
if grep -q $'\r$' "$target_config"; then
    normalized_config=$(mktemp)
    trap 'rm -f -- "$normalized_config"' EXIT
    sed 's/\r$//' "$target_config" > "$normalized_config"
    target_config=$normalized_config
fi
if [[ -n $(git -C "$src" ls-files -- target/linux/sd5113) ]]; then
    echo "The OpenWrt checkout already tracks target/linux/sd5113." >&2
    exit 1
fi
cp -a -- "$overlay/target/linux/sd5113" "$src/target/linux/"
chmod 0755 "$src/target/linux/sd5113/base-files/lib/hg8245/ram-init"
chmod 0755 "$src/target/linux/sd5113/base-files/lib/hg8245/installer-init"
chmod 0644 "$src/target/linux/sd5113/base-files/etc/board.d/01_leds"
chmod 0755 "$src/target/linux/sd5113/base-files/etc/board.d/02_network"
chmod 0755 "$src/target/linux/sd5113/base-files/usr/sbin/hg8245-factory-backup"
if grep -Fxq "CONFIG_PACKAGE_sd5113-omcid=y" "$target_config"; then
    cp -a -- "$overlay/package/network/sd5113-omcid" "$src/package/network/"
fi
with_le88226_id=0
with_spi_audit=0
with_spi_master=0
with_zsp_lab=0
with_voice_chain=0
with_board_io=0
with_gpon_rx=0
with_fxs=0
if grep -Eq '^CONFIG_PACKAGE_(kmod-le88266|hg8245-sip|luci-app-hg8245(-telephony)?)=y$' "$target_config"; then
    with_fxs=1
    for path in kernel/le88266 kernel/spi-sd5113; do
        cp -a -- "$overlay/package/$path" "$src/package/${path%/*}/"
    done
    cp -- "$project"/tools/hg8245-zsp/zsp-{pcm,service,hardware,hif,loader}.[ch] "$src/package/kernel/le88266/src/"
    mkdir -p -- "$src/package/kernel/le88266/firmware"
    stock_args=()
    if [[ -n ${HG8245_STOCK_DUMP:-} ]]; then stock_args+=(--stock-nor "$HG8245_STOCK_DUMP"); fi
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/hg8245-zsp/extract-stock.py" \
        "$src/package/kernel/le88266/firmware/zsp_min.out" "${stock_args[@]}"
fi
check_board_io_config() {
    local config=$1 package
    for package in input-gpio-keys-polled input-evdev leds-gpio i2c-sd5113 \
                   spi-sd5113 hg8245-le88226-id hg8245-zsp-lab; do
        if ! grep -Fx "CONFIG_PACKAGE_kmod-$package=y" "$config"; then
            echo "Missing board I/O package: kmod-$package" >&2
            return 1
        fi
    done
    grep -Fx 'CONFIG_PACKAGE_hg8245-optic-id=y' "$config"
    if grep -Eq '^CONFIG_PACKAGE_kmod-(gpio-button-hotplug|spi-dev|hg8245-spi-audit)=[ym]$' "$config"; then
        echo "Board I/O profile requires native SPI; no spidev, direct audit or button hotplug." >&2
        exit 1
    fi
}
if grep -Fxq 'CONFIG_PACKAGE_kmod-input-gpio-keys-polled=y' "$target_config"; then
    with_board_io=1
    check_board_io_config "$target_config"
    pkg="$src/package/kernel/i2c-sd5113"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/i2c-sd5113) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/i2c-sd5113." >&2
        exit 1
    fi
    mkdir -p -- "$pkg"
    cp -a -- "$overlay/package/kernel/i2c-sd5113/." "$pkg/"
    pkg="$src/package/utils/hg8245-optic-id"
    if [[ -n $(git -C "$src" ls-files -- package/utils/hg8245-optic-id) ]]; then
        echo "The OpenWrt checkout already tracks package/utils/hg8245-optic-id." >&2
        exit 1
    fi
    mkdir -p -- "$pkg"
    cp -a -- "$overlay/package/utils/hg8245-optic-id/." "$pkg/"
fi
if grep -Eq '^CONFIG_PACKAGE_(kmod-hpsf2120|luci-app-sd5113-gpon|luci-app-hg8245)=y$' "$target_config"; then
    for path in kernel/hpsf2120 kernel/i2c-sd5113 utils/hg8245-optic-id; do
        cp -a -- "$overlay/package/$path" "$src/package/${path%/*}/"
    done
fi
if grep -Eq '^CONFIG_PACKAGE_(kmod-sd5113-gpon-rx|luci-app-sd5113-gpon|luci-app-hg8245)=y$' "$target_config"; then
    with_gpon_rx=1
    pkg="$src/package/kernel/sd5113-gpon-rx"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/sd5113-gpon-rx) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/sd5113-gpon-rx." >&2
        exit 1
    fi
    mkdir -p -- "$pkg"
    cp -a -- "$overlay/package/kernel/sd5113-gpon-rx/." "$pkg/"
fi
check_spi_master_config() {
    if grep -Eq '^CONFIG_PACKAGE_kmod-(spi-dev|hg8245-spi-audit)=[ym]$' "$1"; then
        echo "The SPI master RAM profile cannot include spidev or direct SPI audit." >&2
        exit 1
    fi
    if [[ $with_voice_chain == 0 ]] && grep -Eq '^CONFIG_PACKAGE_kmod-hg8245-zsp-lab=[ym]$' "$1"; then
        echo "Combining SPI master and ZSP requires the explicit LE88226 voice-chain profile." >&2
        exit 1
    fi
}
if grep -Fxq 'CONFIG_PACKAGE_kmod-hg8245-le88226-id=y' "$target_config"; then
    with_le88226_id=1
    if grep -Fxq 'CONFIG_PACKAGE_kmod-hg8245-zsp-lab=y' "$target_config"; then
        with_voice_chain=1
    fi
fi
if [[ $with_le88226_id == 1 ]] || grep -Fxq 'CONFIG_PACKAGE_kmod-spi-sd5113=y' "$target_config"; then
    with_spi_master=1
    check_spi_master_config "$target_config"
    pkg="$src/package/kernel/spi-sd5113"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/spi-sd5113) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/spi-sd5113." >&2
        exit 1
    fi
    mkdir -p -- "$pkg"
    cp -a -- "$overlay/package/kernel/spi-sd5113/." "$pkg/"
fi
if [[ $with_le88226_id == 1 ]]; then
    pkg="$src/package/kernel/hg8245-le88226-id"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/hg8245-le88226-id) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/hg8245-le88226-id." >&2
        exit 1
    fi
    id_package="$overlay/package/kernel/hg8245-le88226-id"
    for file in src/Makefile src/hg8245_le88226_id.c; do
        if [[ ! -f "$id_package/$file" ]]; then
            echo "Missing LE88226 ID module source: $id_package/$file" >&2
            exit 1
        fi
    done
    mkdir -p -- "$pkg"
    cp -a -- "$id_package/." "$pkg/"
fi
if grep -Fxq 'CONFIG_PACKAGE_kmod-hg8245-spi-audit=y' "$target_config"; then
    with_spi_audit=1
    pkg="$src/package/kernel/hg8245-spi-audit"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/hg8245-spi-audit) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/hg8245-spi-audit." >&2
        exit 1
    fi
    mkdir -p -- "$pkg/src"
    cp -a -- "$overlay/package/kernel/hg8245-spi-audit/." "$pkg/"
    cp -- "$project/tools/hg8245-slicprobe/Makefile" \
        "$project/tools/hg8245-slicprobe/hg8245_slicprobe.c" "$pkg/src/"
fi
if grep -Fxq 'CONFIG_PACKAGE_kmod-hg8245-zsp-lab=y' "$target_config"; then
    with_zsp_lab=1
    pkg="$src/package/kernel/hg8245-zsp-lab"
    if [[ -n $(git -C "$src" ls-files -- package/kernel/hg8245-zsp-lab) ]]; then
        echo "The OpenWrt checkout already tracks package/kernel/hg8245-zsp-lab." >&2
        exit 1
    fi
    mkdir -p -- "$pkg/src" "$pkg/firmware"
    cp -a -- "$overlay/package/kernel/hg8245-zsp-lab/." "$pkg/"
    cp -- "$project/tools/hg8245-zsp/Makefile" "$project/tools/hg8245-zsp/zsp-stage-module.c" \
        "$project"/tools/hg8245-zsp/zsp-{loader,hif,hardware,pcm}.[ch] "$pkg/src/"
    cp -- "$project/tools/hg8245-slicprobe/hg8245_slicprobe.c" \
        "$project/tools/hg8245-slicprobe/slicprobe-embedded.h" "$pkg/src/"
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/hg8245-zsp/extract-stock.py" \
        "$pkg/firmware/zsp_min.out"
fi
cd -- "$src"

with_leds=0
if grep -qx 'CONFIG_PACKAGE_hg8245-leds=y' "$target_config"; then
    with_leds=1
    cp -a -- "$overlay/package/utils/hg8245-leds" "$src/package/utils/"
fi
with_panel=0
panel_apps=(luci-app-hg8245-telephony luci-app-sd5113-gpon luci-app-ledtrig-hg8245)
if grep -Eq '^CONFIG_PACKAGE_luci-app-(hg8245(-telephony)?|sd5113-gpon|ledtrig-hg8245)=y$' "$target_config"; then
    with_panel=1
    for pkg in "${panel_apps[@]}" luci-app-hg8245 hg8245-sip hg8245-leds; do
        cp -a -- "$overlay/package/utils/$pkg" "$src/package/utils/"
    done
    for pkg in "${panel_apps[@]}"; do
        find "$src/package/utils/$pkg" -type d -exec chmod 0755 {} +
        find "$src/package/utils/$pkg" -type f -exec chmod 0644 {} +
    done
    chmod 0755 "$src"/package/utils/luci-app-{hg8245-telephony,sd5113-gpon}/root/usr/libexec/rpcd/*
fi
with_luci=0
if grep -qx 'CONFIG_PACKAGE_luci-light=y' "$target_config"; then
    with_luci=1
fi
{
    if [[ $with_luci == 1 || $with_panel == 1 ]]; then
        if [[ ${HG8245_OFFLINE:-0} != 1 ]]; then
            scripts/feeds update packages luci
        fi
        if [[ $with_luci == 1 ]]; then
            scripts/feeds install -p luci luci-light
        else
            scripts/feeds install -p luci luci-base luci-theme-bootstrap luci-mod-system
            if grep -Fxq 'CONFIG_PACKAGE_luci-mod-status=y' "$target_config"; then
                scripts/feeds install -p luci luci-mod-status luci-mod-network
            fi
        fi
    fi
    if grep -Fxq 'CONFIG_PACKAGE_igmpproxy=y' "$target_config"; then
        scripts/feeds install -p packages igmpproxy
    fi
    # feeds install can refresh .config while the package list is changing.
    install -m 0644 -- "$target_config" .config
    make defconfig
    grep -Fx 'CONFIG_TARGET_sd5113_generic_DEVICE_sd5113_hg8245=y' .config
    grep -Fx 'CONFIG_TARGET_ROOTFS_INITRAMFS=y' .config
    if [[ $with_fxs == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-le88266=y' .config
    fi
    if [[ $with_board_io == 1 ]]; then
        check_board_io_config .config
    fi
    if [[ $with_gpon_rx == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-sd5113-gpon-rx=y' .config
    fi
    if [[ $with_spi_master == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-spi-sd5113=y' .config
        check_spi_master_config .config
    fi
    if [[ $with_le88226_id == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-hg8245-le88226-id=y' .config
    elif grep -Fxq 'CONFIG_PACKAGE_kmod-hg8245-le88226-id=y' .config; then
        echo "Unexpected LE88226 ID package selection after defconfig." >&2
        exit 1
    fi
    if [[ $with_spi_audit == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-hg8245-spi-audit=y' .config
    fi
    if [[ $with_zsp_lab == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_kmod-hg8245-zsp-lab=y' .config
    fi
    if [[ $with_luci == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_luci-light=y' .config
        grep -Fx 'CONFIG_PACKAGE_uhttpd=y' .config
    fi
    if [[ $with_panel == 1 ]]; then
        for pkg in "${panel_apps[@]}"; do
            selection="CONFIG_PACKAGE_$pkg=y"
            if grep -Fxq "$selection" "$target_config"; then grep -Fx "$selection" .config; fi
        done
    fi
    if [[ $with_leds == 1 ]]; then
        grep -Fx 'CONFIG_PACKAGE_hg8245-leds=y' .config
        grep -Fx 'CONFIG_PACKAGE_kmod-leds-gpio=y' .config
    fi
    if [[ ${HG8245_OFFLINE:-0} != 1 ]]; then
        make -j "$jobs" download
    fi
    make -j "$jobs" V=s
} 2>&1 | tee "$work/build.log"

shopt -s nullglob
images=(bin/targets/sd5113/generic/*sd5113_hg8245-initramfs-kernel.bin)
if [[ ${#images[@]} != 1 ]]; then
    echo "Expected exactly one HG8245 initramfs kernel image; found ${#images[@]}." >&2
    exit 1
fi
image=${images[0]}
python3 "$project/tools/verify_ram_image.py" "$image"
single_feature=
if [[ $with_board_io == 0 ]]; then
    if [[ $with_fxs == 1 && $with_gpon_rx == 0 ]]; then single_feature=telephony; fi
    if [[ $with_fxs == 0 && $with_gpon_rx == 1 ]]; then single_feature=gpon; fi
fi
if [[ -n "$single_feature" ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_fxs_image.py" --single-feature "$single_feature" "$image"
elif [[ $with_fxs == 1 ]]; then
    fxs_args=()
    if grep -Fxq "CONFIG_PACKAGE_ppp-mod-pppoe=y" "$target_config"; then fxs_args+=(--pppoe); fi
    if [[ $with_leds == 1 ]]; then
        fxs_args+=(--front-panel)
        if grep -Fxq 'CONFIG_PACKAGE_hostapd-mbedtls=y' "$target_config"; then
            fxs_args+=(--panel-wifi)
        fi
    fi
    if [[ $with_panel == 1 ]]; then
        fxs_args+=(--ethernet-panel)
        for pkg in "${panel_apps[@]}"; do
            if grep -Fxq "CONFIG_PACKAGE_$pkg=y" .config; then fxs_args+=(--app "$pkg"); fi
        done
    elif [[ $with_luci == 0 ]]; then
        fxs_args+=(--lite)
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_fxs_image.py" "${fxs_args[@]}" "$image"
elif [[ $with_gpon_rx == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_board_io_image.py" --require-gpio-map --gpon-queue "$image"
elif [[ $with_board_io == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_board_io_image.py" --require-gpio-map "$image"
elif [[ $with_voice_chain == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_spi_master_image.py" --voice-chain --gpio-func --cs0-pads-only "$image"
elif [[ $with_le88226_id == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_spi_master_image.py" --le88226-id --gpio-func --cs0-pads-only "$image"
elif [[ $with_spi_master == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_spi_master_image.py" --gpio-func --cs0-pads-only "$image"
fi
if [[ $with_spi_audit == 1 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_spi_uart_image.py" "$image"
fi
if [[ $with_zsp_lab == 1 && $with_voice_chain == 0 ]]; then
    PYTHONDONTWRITEBYTECODE=1 python3 "$project/tools/verify_zsp_ram_image.py" "$image"
fi
output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245"}
name=${image##*/}
if [[ $with_le88226_id == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-le88226-id"}
    name=hg8245-le88226-id.bin
elif [[ $with_spi_master == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-spi-master"}
    name=hg8245-spi-master.bin
fi
if [[ $with_spi_audit == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-spi-uart"}
    name=${HG8245_SPI_AUDIT_NAME:-hg8245-spi-uart.bin}
    if [[ ! $name =~ ^hg8245-spi-[a-z0-9-]+\.bin$ ]]; then
        echo "Invalid diagnostic image name: $name" >&2
        exit 1
    fi
fi
if [[ $with_fxs == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-fxs-pon-r6"}
    name=voice-pon-r6.bin
    if [[ $with_panel == 1 ]]; then
        output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-ethernet-fxs"}
        name=voice-lan.bin
        if [[ $with_leds == 1 ]]; then
            output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-panel-r12"}
            name=voice-panel-r12.bin
            if grep -Fxq 'CONFIG_PACKAGE_hostapd-mbedtls=y' "$target_config"; then
                output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-panel-wifi-r12"}
                name=voice-panel-wifi-r12.bin
            fi
        fi
    elif [[ $with_luci == 0 ]]; then
        output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-fxs-pon-lite-r6"}
        name=voice-pon-lite-r6.bin
    fi
elif [[ $with_gpon_rx == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-gpon-queue"}
    name=gpon-queue.bin
elif [[ $with_board_io == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-gpio-map"}
    name=gpio-map.bin
elif [[ $with_voice_chain == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-voice-chain"}
    name=${HG8245_VOICE_CHAIN_NAME:-hg8245-voice-chain.bin}
    if [[ ! $name =~ ^hg8245-voice-chain(-[a-z0-9-]+)?\.bin$ ]]; then
        echo "Invalid voice-chain image name: $name" >&2
        exit 1
    fi
elif [[ $with_zsp_lab == 1 ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-zsp-boot"}
    name=${HG8245_ZSP_LAB_NAME:-hg8245-zsp-boot.bin}
    if [[ ! $name =~ ^hg8245-zsp-[a-z0-9-]+\.bin$ ]]; then
        echo "Invalid ZSP lab image name: $name" >&2
        exit 1
    fi
fi
if [[ -n "$single_feature" ]]; then
    output=${HG8245_ARTIFACT_DIR:-"$project/artifacts/hg8245-$single_feature"}
    name="hg8245-$single_feature.bin"
fi
mkdir -p -- "$output"
cp -- "$image" "$output/$name"
files=("$name")
if [[ $with_fxs == 1 || -n "$single_feature" ]]; then
    drivers=(le88266 hpsf2120 sd5113-gpon-rx)
    if [[ $with_leds == 1 ]]; then drivers+=(ledtrig-pattern); fi
    for driver in "${drivers[@]}"; do
        grep -Fxq "CONFIG_PACKAGE_kmod-$driver=y" .config || continue
        version=$(awk -v pkg="kmod-$driver" '$1 == pkg && $2 == "-" {print $3}' "${image%-initramfs-kernel.bin}.manifest")
        packages=(bin/targets/sd5113/generic/packages/kmod-"${driver}_${version}"_*.ipk)
        [[ ${#packages[@]} == 1 ]] || { echo "Expected one package for $driver" >&2; exit 1; }
        cp -- "${packages[0]}" "$output/"
        files+=("${packages[0]##*/}")
    done
fi
if [[ $with_panel == 1 ]]; then
    for pkg in "${panel_apps[@]}" hg8245-leds hg8245-sip hg8245-rpc-compat; do
        grep -Fxq "CONFIG_PACKAGE_$pkg=y" .config || continue
        version=$(awk -v pkg="$pkg" '$1 == pkg && $2 == "-" {print $3}' "${image%-initramfs-kernel.bin}.manifest")
        packages=(bin/packages/*/*/"${pkg}_${version}_"*.ipk)
        [[ ${#packages[@]} == 1 ]] || { echo "Expected one package for $pkg $version" >&2; exit 1; }
        cp -- "${packages[0]}" "$output/"
        files+=("${packages[0]##*/}")
    done
fi
cd -- "$output"
sha256sum -- "${files[@]}" | tee SHA256SUMS
printf 'Image: %s/%s\n' "$output" "$name"

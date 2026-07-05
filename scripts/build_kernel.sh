#!/usr/bin/env bash
# Build mainline kernel Image + r8q DTB.
# Usage: ./build_kernel.sh [clean]
set -euo pipefail

cd "$(dirname "$0")/kernel"
export ARCH=arm64 LLVM=1
OUT=../out

[ "${1:-}" = "clean" ] && rm -rf "$OUT"

make O="$OUT" defconfig
../kernel/scripts/kconfig/merge_config.sh -O "$OUT" -m "$OUT/.config" ../r8q_bringup.config
make O="$OUT" olddefconfig

make O="$OUT" -j"$(nproc)" Image.gz dtbs
make O="$OUT" -j"$(nproc)" modules

ls -la "$OUT/arch/arm64/boot/Image.gz" "$OUT/arch/arm64/boot/dts/qcom/sm8250-samsung-r8q.dtb"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the Android bionic KGSL Vulkan HAL. No system/vendor installation.
set -euo pipefail
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29 or compatible}"
source_root=$(cd "$(dirname "$0")/.." && pwd)
build_root=${1:?Usage: build-android-turnip-kgsl.sh BUILD_DIRECTORY}
mkdir -p "$build_root"
build_root=$(cd "$build_root" && pwd)
case "$(uname -s)" in
  Darwin) host_tag=darwin-x86_64 ;;
  Linux) host_tag=linux-x86_64 ;;
  *) echo 'Unsupported NDK host' >&2; exit 2 ;;
esac
toolchain="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$host_tag/bin"
cat > "$build_root/android-aarch64.ini" <<EOF
[binaries]
c = '$toolchain/aarch64-linux-android33-clang'
cpp = '$toolchain/aarch64-linux-android33-clang++'
ar = '$toolchain/llvm-ar'
strip = '$toolchain/llvm-strip'
pkg-config = 'false'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF
setup_args=()
if [[ -f "$build_root/native/meson-private/coredata.dat" ]]; then
  setup_args+=(--reconfigure --clearcache)
fi
meson setup "${setup_args[@]}" "$build_root/native" "$source_root" \
  --cross-file "$build_root/android-aarch64.ini" --buildtype release \
  -Dplatforms=android -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Dplatform-sdk-version=33 -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dgallium-drivers= -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dllvm=disabled -Dbuild-tests=false -Dzstd=disabled
ninja -C "$build_root/native" -j "${TURNIP_BUILD_JOBS:-8}"
printf '%s\n' "$build_root/native/src/freedreno/vulkan/libvulkan_freedreno.so"

#!/usr/bin/env bash
set -euo pipefail

: "${IZTWINS2_SDK:?Set IZTWINS2_SDK to the Ounce SDK directory}"
: "${IZTWINS2_BUILD:?Set IZTWINS2_BUILD to the build directory}"
: "${IZTWINS2_PACKER:?Set IZTWINS2_PACKER to the IZ2S relocatable.py tool}"
: "${IZTWINS2_HOST_TOOLS:?Set IZTWINS2_HOST_TOOLS to the hosted-engine tools directory}"
: "${IZTWINS2_PLATFORM:?Set IZTWINS2_PLATFORM to the IZ2S platform checkout}"
if [[ $# != 1 ]]; then echo 'Usage: build-iza.sh <output directory>' >&2; exit 2; fi
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
sdk=$(realpath "$IZTWINS2_SDK")
build=$(realpath -m "$IZTWINS2_BUILD")
export DEVKITPRO="$sdk/devkitpro"
stage="$build/iza"
cross="$sdk/devkitpro/devkitA64/bin/aarch64-none-elf-"
mkdir -p "$stage/ffmpeg" "$1"

jobs=${IZTWINS2_JOBS:-6}
(
    cd "$stage/ffmpeg"
    if [[ ! -f config.h ]]; then
        "$root/ffmpeg/configure" --arch=aarch64 --cpu=cortex-a78c --target-os=none --cross-prefix="$cross" \
            --enable-cross-compile --disable-autodetect --disable-network --disable-everything \
            --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter \
            --disable-pthreads --disable-runtime-cpudetect --enable-pic --enable-avformat --enable-avcodec \
            --enable-swscale --enable-swresample --enable-demuxer=mpegts,mov --enable-parser=h264,aac \
            --enable-decoder=h264,aac,ccaption \
            --extra-cflags="-D__SWITCH__ -D__SWITCH2__ -isystem $DEVKITPRO/libounce/include -mtp=soft -mbranch-protection=bti -ffunction-sections -fdata-sections" \
            --extra-ldflags="-specs=$DEVKITPRO/libounce/switch.specs -L$DEVKITPRO/libounce/lib -mtp=soft -Wl,--gc-sections -Wl,-z,notext" \
            --extra-libs=-lounce
    fi
    make -j"$jobs"
)
bash "$here/../native/build-text-deps.sh" "$cross" "$root/textdeps" "$stage/textdeps" "$jobs"
python3 "$here/../native/font-assets.py" "$build"
python3 "$here/../native/branding.py" "$here/../native/Roboto-Bold.ttf" "$build" --full-hd
"$here/build-engine.sh"
python3 -B "$IZTWINS2_HOST_TOOLS/verify-hosted-bti.py" "$IZTWINS2_PACKER" "$stage/engine.elf"
python3 -B "$IZTWINS2_HOST_TOOLS/verify-hosted-tls.py" "$IZTWINS2_PACKER" "$stage/engine.elf" "$stage/engine-checked.elf"
python3 -B "$IZTWINS2_PACKER" --elf "$stage/engine-checked.elf" --output "$stage/engine-reloc.izdata"
python3 -B "$IZTWINS2_HOST_TOOLS/portable-image.py" "$stage/engine-reloc.izdata" "$stage/engine.izdata"
"${cross}gcc" -Os -fpie -ffreestanding -fno-stack-protector -mbranch-protection=bti -Wall -Wextra -Werror \
    -I"$sdk/sb4/include" -Wl,-T,"$here/frontend.ld" -nostdlib "$here/frontend.c" -o "$stage/frontend.elf"
"${cross}objcopy" -O binary "$stage/frontend.elf" "$stage/native.bin"
for stage_name in vert frag comp; do
    glslc -O "$here/app.$stage_name" -o "$stage/app.$stage_name.spv"
    spirv-val "$stage/app.$stage_name.spv"
done
cp "$build/icon.png" "$stage/icon.png"
node "$here/package.cjs" "$IZTWINS2_PLATFORM" "$stage" "$1"

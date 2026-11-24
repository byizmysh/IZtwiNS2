#!/usr/bin/env bash
set -euo pipefail

: "${DEVKITPRO:?Set DEVKITPRO to the Switch 2 Ounce devkit}"
: "${FFMPEG_SRC:?Set FFMPEG_SRC to the pinned FFmpeg checkout}"

here=$(cd -- "$(dirname -- "$0")" && pwd)
src=$(realpath "$FFMPEG_SRC")
build=$(realpath -m "${1:-build/iztwins2-nro}")
cross="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-"
tools="$DEVKITPRO/tools/bin"
sdk="$DEVKITPRO/libounce"
mkdir -p "$build/ffmpeg"

flags=(-D__SWITCH__ -D__SWITCH2__ -isystem "$sdk/include" -mtp=soft
       -ffunction-sections -fdata-sections)
link=(-specs="$sdk/switch.specs" -L"$sdk/lib" -mtp=soft -Wl,--gc-sections)

(
    cd "$build/ffmpeg"
    if [[ ! -f config.h ]]; then
        "$src/configure" --arch=aarch64 --cpu=cortex-a78c --target-os=none --cross-prefix="$cross" \
            --enable-cross-compile --disable-autodetect --disable-network --disable-everything \
            --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter \
            --disable-pthreads --disable-runtime-cpudetect --enable-pic --enable-avformat --enable-avcodec \
            --enable-swscale --enable-swresample --enable-demuxer=mpegts,mov --enable-parser=h264,aac \
            --enable-decoder=h264,aac,ccaption --extra-cflags="${flags[*]}" \
            --extra-ldflags="${link[*]} -Wl,-z,notext" --extra-libs=-lounce
    fi
    make -j"${IZTWINS2_JOBS:-6}"
)

text_sources=$(realpath "$here/../../textdeps")
text_deps="$build/textdeps"
bash "$here/build-text-deps.sh" "$cross" "$text_sources" "$text_deps" "${IZTWINS2_JOBS:-6}"
python3 "$here/font-assets.py" "$build"
python3 "$here/branding.py" "$here/Roboto-Bold.ttf" "$build"
client_id=${IZTWINS2_CLIENT_ID:-1a0hyv0sxnwpkvde7pjyo4gpwcnwj9}
if [[ ! "$client_id" =~ ^[A-Za-z0-9_-]{1,63}$ ]]; then
    echo 'Invalid Twitch public client ID.' >&2
    exit 2
fi
"${cross}g++" -std=gnu++17 -O2 -Wall -Wextra -Werror -fPIE -mcpu=cortex-a78c+crypto \
    "${flags[@]}" -I"$here/vendor" -c "$here/account-json.cpp" -o "$build/account-json.o"
"${cross}gcc" -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -fPIE -mcpu=cortex-a78c+crypto \
    "${flags[@]}" -I"$here/vendor" -c "$here/preview.c" -o "$build/preview.o"
"${cross}gcc" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIE -mcpu=cortex-a78c+crypto \
    "${flags[@]}" -I"$build" -I"$build/ffmpeg" -I"$src" -I"$here/vendor/qrcodegen" \
    -I"$text_deps/freetype/include/freetype2" -I"$text_deps/fribidi/include/fribidi" -I"$text_sources/harfbuzz/src" \
    -DIZTWINS2_CLIENT_ID=\""$client_id"\" \
    "$here/player.c" "$here/https.c" "$here/twitch.c" "$here/media.c" "$here/webvtt.c" "$here/touch.c" \
    "$here/account.c" "$here/library.c" "$here/menu.c" "$here/preview-cache.c" \
    "$here/chat.c" "$here/chat-protocol.c" "$here/typography.c" "$build/text-assets.S" \
    "$here/vendor/qrcodegen/qrcodegen.c" "$build/account-json.o" "$build/preview.o" \
    "${link[@]}" -Wl,--start-group \
    "$build"/ffmpeg/lib{avformat,avcodec,swscale,swresample,avutil}/*.a \
    "$text_deps"/freetype/lib/libfreetype.a "$text_deps"/fribidi/lib/libfribidi.a "$text_deps"/libharfbuzz.a \
    -lounce -lstdc++ -lm -Wl,--end-group \
    -o "$build/iztwins2.elf"
"$tools/nacptool" --create 'IZtwiNS2' 'IZtwiNS2 contributors' '0.11.0' "$build/iztwins2.nacp"
asset_args=()
if [[ -n ${IZTWINS2_RETURN_NRO:-} ]]; then
    python3 "$here/verify-return.py" "$IZTWINS2_RETURN_NRO"
    mkdir -p "$build/return-romfs"
    cp -- "$IZTWINS2_RETURN_NRO" "$build/return-romfs/return-iz2s.nro"
    asset_args=(--romfsdir="$build/return-romfs")
fi
"$tools/elf2nro" "$build/iztwins2.elf" "$build/iztwins2.nro" --nacp="$build/iztwins2.nacp" \
    --icon="$build/icon.jpg" "${asset_args[@]}"

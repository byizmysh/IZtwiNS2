#!/usr/bin/env bash
set -euo pipefail

: "${IZTWINS2_SDK:?Set IZTWINS2_SDK to the IZ2S Ounce SDK directory}"
: "${IZTWINS2_BUILD:?Set IZTWINS2_BUILD to the build directory}"
: "${IZTWINS2_HOST_LIBOUNCE:?Set IZTWINS2_HOST_LIBOUNCE to a BTI-enabled libounce archive}"
: "${IZTWINS2_HOST_LIBC:?Set IZTWINS2_HOST_LIBC to the hosted BTI libc directory}"
: "${IZTWINS2_HOST_CXX:?Set IZTWINS2_HOST_CXX to the hosted BTI C++ directory}"
here=$(cd -- "$(dirname -- "$0")" && pwd)
src="$here/../native"
root=$(cd -- "$here/../.." && pwd)
build=$(realpath "$IZTWINS2_BUILD")
sdk=$(realpath "$IZTWINS2_SDK")
devkitpro="$sdk/devkitpro"
cross="$devkitpro/devkitA64/bin/aarch64-none-elf-"
output="$build/iza"
mkdir -p "$output"

flags=(-O2 -Wall -Wextra -Werror -Wno-unused-function -fPIE -mtp=soft -mcpu=cortex-a78c+crypto
    -ffunction-sections -fdata-sections -mbranch-protection=bti -D__SWITCH__ -D__SWITCH2__ -DIZTWINS2_IZA
    -isystem "$devkitpro/libounce/include" -I"$sdk/sb4/include" -I"$build" -I"$output/ffmpeg"
    -I"$root/ffmpeg" -I"$src/vendor" -I"$src/vendor/qrcodegen"
    -I"$output/textdeps/freetype/include/freetype2"
    -I"$output/textdeps/fribidi/include/fribidi" -I"$root/textdeps/harfbuzz/src")
client_id=${IZTWINS2_CLIENT_ID:-1a0hyv0sxnwpkvde7pjyo4gpwcnwj9}
[[ "$client_id" =~ ^[A-Za-z0-9_-]{1,63}$ ]]
flags+=(-DIZTWINS2_CLIENT_ID=\""$client_id"\")

objects=()
for file in "$here"/{engine,engine-tls,native-audio,native-storage}.c "$sdk"/sb4/source/{host-posix,host-runtime,host-heap}.c \
    "$src"/{player,https,twitch,media,webvtt,account,library,menu,preview,preview-cache,chat,chat-protocol,typography,touch}.c \
    "$src/vendor/qrcodegen/qrcodegen.c"; do
    object="$output/$(basename "${file%.c}").o"
    "${cross}gcc" -std=gnu11 "${flags[@]}" -c "$file" -o "$object"
    objects+=("$object")
done
"${cross}g++" -std=gnu++17 "${flags[@]}" -c "$src/account-json.cpp" -o "$output/account-json.o"
"${cross}gcc" -c "$here/engine-tls.S" -o "$output/tls-entry.o"
"${cross}gcc" -c "$build/text-assets.S" -o "$output/text-assets.o"
wraps=()
for list in host-runtime host-heap; do
    while IFS= read -r symbol; do
        [[ -z "$symbol" ]] || wraps+=("-Wl,--wrap=$symbol")
    done < "$sdk/sb4/$list.symbols"
done
while IFS= read -r symbol; do
    wraps+=("-Wl,--wrap=$symbol")
done < <(sed -nE 's/^SB4_POSIX(_[A-Z]+)?\(([a-zA-Z0-9_]+),.*/\2/p' "$sdk/sb4/source/host-posix.def")
wraps+=(-Wl,--wrap=__aarch64_read_tp)
for symbol in threadCreate threadStart threadClose threadWaitForExit swkbdCreate swkbdClose swkbdShow \
    swkbdConfigMakePresetDefault swkbdConfigSetGuideText swkbdConfigSetInitialText __nx_exit exit; do
    wraps+=("-Wl,--wrap=$symbol")
done
"${cross}g++" -mcpu=cortex-a78c+crypto -mtp=soft -fPIE -static -nostartfiles \
    -Wl,--gc-sections,--emit-relocs,--no-relax,--no-undefined -Wl,-T,"$here/engine.ld" \
    -L"$IZTWINS2_HOST_CXX" -L"$IZTWINS2_HOST_LIBC" \
    "${wraps[@]}" "${objects[@]}" "$IZTWINS2_HOST_CXX/witchtube-hosted-unwind.o" \
    "$output/account-json.o" "$output/tls-entry.o" "$output/text-assets.o" \
    -Wl,--start-group \
    "$output"/ffmpeg/lib{avformat,avcodec,swscale,swresample,avutil}/*.a \
    "$output/textdeps/freetype/lib/libfreetype.a" "$output/textdeps/fribidi/lib/libfribidi.a" \
    "$output/textdeps/libharfbuzz.a" "$IZTWINS2_HOST_LIBOUNCE" -lstdc++ -lm -Wl,--end-group \
    -o "$output/engine.elf"

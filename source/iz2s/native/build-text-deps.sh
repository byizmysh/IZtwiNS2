#!/usr/bin/env bash
set -euo pipefail

compiler=$1
deps=$2
output=$3
jobs=$4
mkdir -p "$output"
machine=(-mbranch-protection=bti -mtp=soft -mcpu=cortex-a78c+crypto -fPIC -ffunction-sections -fdata-sections)
flags="-O2 ${machine[*]}"

build_freetype() {
    local prefix="$output/freetype"
    [[ -s "$prefix/lib/libfreetype.a" ]] && return
    mkdir -p "$output/freetype-build"
    cd "$output/freetype-build"
    CC="${compiler}gcc" AR="${compiler}ar" RANLIB="${compiler}ranlib" CFLAGS="$flags" \
        "$deps/freetype/configure" --host=aarch64-none-elf --prefix="$prefix" \
        --disable-shared --enable-static --with-harfbuzz=no --with-png=no --with-zlib=no \
        --with-bzip2=no --with-brotli=no
    make -j"$jobs"
    make install
}

build_fribidi() {
    local prefix="$output/fribidi"
    [[ -s "$prefix/lib/libfribidi.a" ]] && return
    mkdir -p "$output/fribidi-build"
    cd "$output/fribidi-build"
    CC="${compiler}gcc" AR="${compiler}ar" RANLIB="${compiler}ranlib" CFLAGS="$flags" \
        "$deps/fribidi/configure" --host=aarch64-none-elf --prefix="$prefix" \
        --disable-shared --enable-static --disable-docs --disable-bin
    make -j"$jobs"
    make install
}

build_harfbuzz() {
    [[ -s "$output/libharfbuzz.a" ]] && return
    "${compiler}g++" -std=c++17 -O2 "${machine[@]}" -fno-exceptions -fno-rtti \
        -DHB_NO_MT -DHB_NO_MMAP -DHB_NO_GETENV \
        -c "$deps/harfbuzz/src/harfbuzz.cc" -o "$output/harfbuzz.o"
    "${compiler}ar" rcs "$output/libharfbuzz.a" "$output/harfbuzz.o"
}

(build_freetype) > "$output/freetype.log" 2>&1 & ft=$!
(build_fribidi) > "$output/fribidi.log" 2>&1 & bidi=$!
(build_harfbuzz) > "$output/harfbuzz.log" 2>&1 & hb=$!
result=0
for pid in "$ft" "$bidi" "$hb"; do wait "$pid" || result=1; done
if [[ $result != 0 ]]; then
    tail -n 25 "$output"/{freetype,fribidi,harfbuzz}.log >&2
fi
exit "$result"

#!/usr/bin/env bash
# Build RecompTrophy as a PS5 homebrew app folder (eboot.bin, sce_module/libc.prx,
# sce_sys/param.json, fonts/) with the PS5 tooling of ps5-plume-triangle (its
# app CRT, link recipe, libc.prx and FSELF signer), as the ports are built.
#
#   tools/build.sh [ps5-plume-triangle checkout] [title id]
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tri=$(realpath "${1:-$root/../ps5-plume-triangle}")
title_id=${2:-PPSA95400}
sdk_root=${PS5_PAYLOAD_SDK:-$tri/.deps/native/ps5-payload-sdk}
tool=$tri/build/host/ps5-native-tool
build=$root/build
mkdir -p "$build/obj" "$build/link/build"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-19 || command -v clang)}
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$tri/tooling/prospero-clang18" "$@"; }

echo "==> [relay]"
# The relay payload (payload/relay.c), sent to the loader by the app.
"$sdk_root/bin/prospero-clang" -O2 -o "$build/relay.elf" "$root/payload/relay.c"
# The watcher (payload/watcher.c): stays loaded, shows the native notifications.
"$sdk_root/bin/prospero-clang" -O2 -o "$build/watcher.elf" "$root/payload/watcher.c" -lSceNotification
cat > "$build/relay_blob.cpp" <<EOF_BLOB
__asm__(".section .rodata\n.balign 16\n.globl trophies360_relay\ntrophies360_relay:\n"
        ".incbin \"$build/relay.elf\"\n.globl trophies360_relay_end\ntrophies360_relay_end:\n"
        ".balign 16\n.globl trophies360_watcher\ntrophies360_watcher:\n"
        ".incbin \"$build/watcher.elf\"\n.globl trophies360_watcher_end\ntrophies360_watcher_end:\n.previous\n");
EOF_BLOB

echo "==> [compile]"
objs=()
# (third_party/: libmspack's LZX decoder, tiny-AES, stb_image, stb_truetype, miniz)
inc=(-I "$root/src" -I "$root/third_party/stb" -I "$root/third_party/libmspack" -I "$root/third_party/tiny-aes-c")
for src in "$root/third_party/libmspack/lzxd.c" "$root/third_party/tiny-aes-c/aes.c"; do
    obj=$build/obj/$(basename "${src%.c}").c.o
    cc -std=c11 -O2 -w -ffunction-sections -fdata-sections "${inc[@]}" -c "$src" -o "$obj"
    objs+=("$obj")
done
cc -std=c++20 -O2 -c "$build/relay_blob.cpp" -o "$build/obj/relay_blob.o"
objs+=("$build/obj/relay_blob.o")
# (QR codes: Project Nayuki's library, MIT, built with C++ exceptions as it throws them)
cc -std=c++20 -O2 -w -fexceptions -ffunction-sections -fdata-sections -c "$root/src/third_party/qrcodegen/qrcodegen.cpp" \
    -o "$build/obj/qrcodegen.o"
objs+=("$build/obj/qrcodegen.o")
for src in "$root"/src/*.cpp; do
    obj=$build/obj/$(basename "${src%.cpp}").o
    cc -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -fno-exceptions -fno-rtti -ffunction-sections \
        -fdata-sections "${inc[@]}" -c "$src" -o "$obj"
    objs+=("$obj")
done

echo "==> [link]"
# (the ports' recipe, with no RADV: an empty archive in its place)
ar rcs "$build/link/empty.a"
ln -sfn "$tri/tooling" "$build/link/tooling"
ln -sfn "$tri/vendor" "$build/link/vendor"
source "$tri/tools/radv-link.sh"
radv_link_recipe "$build/link" "$sdk_root" "$build/link/empty.a"
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$tri/tooling/native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$build/llvm-pie.elf" \
    "$tri/build/obj/app_crt.o" "$tri/build/obj/app_cpp_runtime.o" \
    "${objs[@]}" "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

echo "==> [package] $title_id"
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
app=$build/app/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/fonts"
"$tool" self --sign --in "$build/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$tri/runtime/libc.prx" "$app/sce_module/libc.prx"
sed "s/@TITLE_ID@/$title_id/g; s/@CONCEPT@/${title_id:4}/g" "$root/sce_sys/param.json.in" > "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds; do
    [[ -f $root/sce_sys/$asset ]] && cp "$root/sce_sys/$asset" "$app/sce_sys/$asset"
done
cp "$root"/fonts/* "$app/fonts/"
# the licenses (the app's GPL-3.0, its libraries' and libmspack's LGPL)
mkdir -p "$app/licenses"
for f in LICENSE THIRD_PARTY_NOTICES.md third_party/libmspack/COPYING.LIB; do
    [[ -f $root/$f ]] && cp "$root/$f" "$app/licenses/"
done
"$tool" self --inspect --file "$app/eboot.bin" | tail -2
echo "==> $app"

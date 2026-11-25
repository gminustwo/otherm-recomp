#!/usr/bin/env bash
# Linux x86_64 build of Other M Recompiled. Run in WSL Ubuntu (or on Linux) after tools\build-otherm.ps1 has
# translated the game on Windows: this reuses that output (generated/) and only compiles it, with the same clang
# version as the Windows build (the pinned LLVM, fetched by tools/fetch-tools-linux.sh).
#   bash tools/build-linux.sh [--jobs N]
# Sources are copied to ~/otherm-linux/src first (WSL compiles far faster off /mnt).
# Output: ~/otherm-linux/build/MetroidOtherM
# Source paths the compiler records (__FILE__ in Mbed TLS and toml11) are mapped to /otherm-linux, so the builder's
# home folder never ships in the program.
# Then tools/package-linux.sh makes the AppImage.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
jobs=8   # a translated shard compile can take ~2.5 GB; WSL gets half the PC's memory
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

work=$HOME/otherm-linux
src=$work/src
[[ -f $root/generated/build_shards/shards.cmake ]] || { echo "no translated game: run tools\\build-otherm.ps1 on Windows first" >&2; exit 1; }
mkdir -p "$src"

tools=$work/portable-tools
if [[ ! -x $tools/toolchain-x86_64/bin/clang++ ]]; then
    if ! command -v unzip >/dev/null; then
        # fetch-tools-linux.sh unpacks Ninja with `unzip -oq <archive> -d <dir>`; WSL's Ubuntu has none by default.
        mkdir -p "$work/shim"
        cat > "$work/shim/unzip" <<'EOF'
#!/bin/sh
archive=""; dir=.
while [ $# -gt 0 ]; do case "$1" in -d) dir=$2; shift ;; -*) ;; *) archive=$1 ;; esac; shift; done
exec python3 -I -m zipfile -e "$archive" "$dir"
EOF
        chmod 755 "$work/shim/unzip"
        export PATH=$work/shim:$PATH
    fi
    bash "$root/tools/fetch-tools-linux.sh" --arch x86_64 --destination "$tools"
fi
bin=$tools/toolchain-x86_64/bin

echo "copying sources to $src"
rsync -a --delete "$root/runtime/" "$src/runtime/"
rsync -a --delete --exclude /build/ "$root/aurora/" "$src/aurora/"
rsync -a --delete --exclude /NOTES.md "$root/project/" "$src/project/"
# The translated shards and data blobs; not the per-function files or the source bundle they were made from.
rsync -a --delete --exclude /functions/ --exclude /base_translation_sources.bin "$root/generated/" "$src/generated/"
# The pinned dependency sources (the Windows-only Dawn build and C++/WinRT stay behind: aurora fetches Linux Dawn).
rsync -a --delete --exclude /dawn_prebuilt/ --exclude /cppwinrt/ "$root/dependencies/" "$src/dependencies/"
# The translator wrote absolute Windows paths into the shard list and the data blobs' .incbin lines.
win=$(wslpath -m "$root" 2>/dev/null || echo "$root")
grep -rlF "$win" "$src/generated" --include='*.cmake' --include='*.S' --include='*.cpp' --include='*.h' |
    xargs -r sed -i "s|$win|$src|g"

deps=()
for dir in "$src"/dependencies/*/; do
    name=$(basename "$dir")
    deps+=("-DFETCHCONTENT_SOURCE_DIR_${name^^}=${dir%/}")
done

# A build folder configured from another source folder can't be reused.
if [[ -f $work/build/CMakeCache.txt ]] && ! grep -qF "CMAKE_HOME_DIRECTORY:INTERNAL=$src/runtime" "$work/build/CMakeCache.txt"; then
    echo "the build folder was configured from another source folder: starting it again"
    rm -rf "$work/build"
fi

"$bin/cmake" -S "$src/runtime" -B "$work/build" -G Ninja -DCMAKE_MAKE_PROGRAM="$bin/ninja" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++" -DCMAKE_ASM_COMPILER="$bin/clang" \
    -DCMAKE_AR="$bin/llvm-ar" -DCMAKE_RANLIB="$bin/llvm-ranlib" -DCMAKE_LINKER="$bin/ld.lld" \
    -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor -DCMAKE_POLICY_DEFAULT_CMP0168=NEW \
    -DCMAKE_ASM_FLAGS="-Wa,--noexecstack -ffile-prefix-map=$work=/otherm-linux" \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$work=/otherm-linux" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$work=/otherm-linux" \
    -DOTHERM_TRANSLATED_COMPILE_JOBS="$jobs" "${deps[@]}"
"$bin/cmake" --build "$work/build" --target OtherMRecompiled --parallel "$jobs"
echo "DONE: $work/build/MetroidOtherM"

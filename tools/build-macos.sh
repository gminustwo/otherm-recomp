#!/usr/bin/env bash
# macOS (Apple Silicon) build of Other M Recompiled. Run after tools\build-otherm.ps1 has translated the game on
# Windows and its generated\ and dependencies\ folders have been copied into this checkout (generated\functions\,
# generated\base_translation_sources.bin, dependencies\dawn_prebuilt\ and dependencies\cppwinrt\ aren't needed): this
# reuses that output and only compiles it, with Xcode's Apple Clang and CMake + Ninja (brew install cmake ninja).
#   bash tools/build-macos.sh [--jobs N]
# Sources are copied to ~/otherm-macos/src first (the translator's absolute Windows paths are rewritten there).
# Output: ~/otherm-macos/build/MetroidOtherM
# Source paths the compiler records (__FILE__ in Mbed TLS and toml11) are mapped to /otherm-macos, so the builder's
# home folder never ships in the program.
# Then tools/package-macos.sh makes the app.
# Written for the bash 3.2 macOS ships.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# A translated shard compile can take ~2.5 GB: at most one per 3 GB of memory, and one per core.
cores=$(sysctl -n hw.ncpu)
jobs=$(( $(sysctl -n hw.memsize) / (3 * 1024 * 1024 * 1024) ))
(( jobs > cores )) && jobs=$cores
(( jobs < 1 )) && jobs=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

[[ $(uname -m) == arm64 ]] || { echo "this builds for Apple Silicon (arm64) on an Apple Silicon Mac" >&2; exit 1; }
shards=$root/generated/build_shards/shards.cmake
[[ -f $shards ]] || { echo "no translated game: copy generated\\ from the Windows build (tools\\build-otherm.ps1)" >&2; exit 1; }
[[ -d $root/dependencies ]] || { echo "no dependencies\\: copy it from the Windows build too" >&2; exit 1; }
for tool in cmake ninja rsync xcrun; do
    command -v $tool >/dev/null || { echo "$tool is missing (cmake and ninja: brew install cmake ninja)" >&2; exit 1; }
done

work=$HOME/otherm-macos
src=$work/src
mkdir -p "$src"

echo "copying sources to $src"
rsync -a --delete "$root/runtime/" "$src/runtime/"
rsync -a --delete --exclude /build/ "$root/aurora/" "$src/aurora/"
rsync -a --delete --exclude /NOTES.md "$root/project/" "$src/project/"
# The translated shards and data blobs; not the per-function files or the source bundle they were made from.
rsync -a --delete --exclude /functions/ --exclude /base_translation_sources.bin "$root/generated/" "$src/generated/"
# The pinned dependency sources (the Windows-only Dawn build and C++/WinRT stay behind: aurora fetches macOS Dawn).
rsync -a --delete --exclude /dawn_prebuilt/ --exclude /cppwinrt/ "$root/dependencies/" "$src/dependencies/"

# The translator wrote the Windows checkout's absolute path (C:/.../otherm-recomp) into the shard list and the data
# blobs' .incbin lines: it is read from the shard list and pointed at the copy here.
win=$(grep -oE '[A-Za-z]:[/\\][^"]*[/\\]generated[/\\]' "$src/generated/build_shards/shards.cmake" | head -n 1 |
    sed -E 's#[/\\]generated[/\\]$##' || true)
if [[ -n $win ]]; then
    echo "rewriting $win -> $src"
    win_fwd=${win//\\//}
    froms=("$win")
    [[ $win_fwd != "$win" ]] && froms+=("$win_fwd")
    for from in "${froms[@]}"; do
        # grep finds nothing in files already rewritten: not a failure.
        { grep -rlF "$from" "$src/generated" --include='*.cmake' --include='*.S' --include='*.cpp' --include='*.h' || true; } |
            while IFS= read -r f; do
                FROM="$from" TO="$src" perl -pi -e 's/\Q$ENV{FROM}\E/$ENV{TO}/g' "$f"
            done
    done
fi
if grep -rqE '[A-Za-z]:[/\\][^"]*otherm' "$src/generated" --include='*.cmake' --include='*.S'; then
    grep -rnE '[A-Za-z]:[/\\][^"]*otherm' "$src/generated" --include='*.cmake' --include='*.S' | head -5 >&2
    echo "Windows paths left in the translated game" >&2
    exit 1
fi

deps=()
for dir in "$src"/dependencies/*/; do
    name=$(basename "$dir")
    upper=$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]')
    deps+=("-DFETCHCONTENT_SOURCE_DIR_${upper}=${dir%/}")
done

# A build folder configured from another source folder can't be reused.
if [[ -f $work/build/CMakeCache.txt ]] && ! grep -qF "CMAKE_HOME_DIRECTORY:INTERNAL=$src/runtime" "$work/build/CMakeCache.txt"; then
    echo "the build folder was configured from another source folder: starting it again"
    rm -rf "$work/build"
fi

map="-ffile-prefix-map=$work=/otherm-macos"
# Homebrew's libraries (libpng) are built for the macOS they were installed on: they're kept out, so the pinned
# sources in dependencies/ are built into the program and the app runs on macOS 12 and later.
cmake -S "$src/runtime" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_C_COMPILER="$(xcrun -f clang)" -DCMAKE_CXX_COMPILER="$(xcrun -f clang++)" \
    -DCMAKE_ASM_COMPILER="$(xcrun -f clang)" \
    -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor -DCMAKE_POLICY_DEFAULT_CMP0168=NEW \
    -DCMAKE_ASM_FLAGS="$map" -DCMAKE_C_FLAGS="$map" -DCMAKE_CXX_FLAGS="$map" \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -UPNG_LIBRARY_RELEASE -UPNG_PNG_INCLUDE_DIR \
    -DOTHERM_TRANSLATED_COMPILE_JOBS="$jobs" ${deps[@]+"${deps[@]}"}
cmake --build "$work/build" --target OtherMRecompiled --parallel "$cores"
echo "DONE: $work/build/MetroidOtherM"

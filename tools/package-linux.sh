#!/usr/bin/env bash
# Packs the Linux build (tools/build-linux.sh) into an AppImage for the Steam Deck and other Linux desktops:
#   bash tools/package-linux.sh --version v0.1.0 [--runtime <AppImage type 2 runtime-x86_64>]
# Output: dist/OtherMRecompiled-<version>-x86_64.AppImage (the name the in-game updater fetches) and dist/SHA256SUMS.txt.
# The AppImage holds the program (the game's code in recompiled form, for the player whose disc it was built from) and
# the port's own files, never the disc's: the player's disc image goes next to the AppImage and the first start sets it
# up into Disc/DATA there; UserData (saves, settings, logs) and Metroid.ini live beside it too.
# Needs mksquashfs, patchelf, objcopy and curl. Downloads (once, into ~/otherm-linux): the AppImage runtime
# (github.com/AppImage/type2-runtime) and nodtool (github.com/encounter/nod, MIT / Apache-2.0; reads .rvz/.wia/.gcz).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$HOME/otherm-linux
build=$work/build
version=""
runtime=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version=$2; shift ;;
        --runtime) runtime=$(realpath "$2"); shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done
[[ $version =~ ^v[0-9]+\.[0-9]+\.[0-9]+[a-z]?$ ]] || { echo "--version vX.Y.Z is required" >&2; exit 1; }
exe=$build/MetroidOtherM
[[ -x $exe ]] || { echo "no $exe: run tools/build-linux.sh first" >&2; exit 1; }

nod_version=v2.0.0-alpha.10   # the same pin as tools/build-otherm.ps1
nodtool=$work/nodtool-$nod_version
if [[ ! -x $nodtool ]]; then
    curl -fL -o "$nodtool.tmp" "https://github.com/encounter/nod/releases/download/$nod_version/nodtool-linux-x86_64"
    curl -fL -o "$work/nod-LICENSE-MIT" "https://raw.githubusercontent.com/encounter/nod/$nod_version/LICENSE-MIT"
    curl -fL -o "$work/nod-LICENSE-APACHE" "https://raw.githubusercontent.com/encounter/nod/$nod_version/LICENSE-APACHE"
    echo "f853b83268b9542faf0d28815bc3a23c090ae28ebf397e46f99cbb08122c8307  $nodtool.tmp" | sha256sum -c --quiet ||
        { rm -f "$nodtool.tmp"; echo "nodtool did not match its pinned hash" >&2; exit 1; }
    chmod 755 "$nodtool.tmp" && mv "$nodtool.tmp" "$nodtool"
fi
if [[ -z $runtime ]]; then
    runtime=$work/runtime-x86_64
    [[ -f $runtime ]] || curl -fL -o "$runtime" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
fi

app=$work/AppDir
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib" "$app/usr/share/otherm-recomp/licenses" "$app/usr/share/applications" \
    "$app/usr/share/icons/hicolor/512x512/apps"

# The program without its DWARF (the dynamic symbols stay, for crash stacks); the build's own shared libraries
# (Dawn, SDL3, libpng when shared) and the system's that a desktop may lack, found through $ORIGIN/../lib (an rpath,
# not LD_LIBRARY_PATH, so programs the game starts, such as curl for the updater, use their own libraries).
objcopy --strip-debug "$exe" "$app/usr/bin/MetroidOtherM"
patchelf --set-rpath '$ORIGIN/../lib' "$app/usr/bin/MetroidOtherM"
ldd "$exe" | awk '/=>/ {print $3}' | while read -r lib; do
    case "$lib" in
        "$build"/* | */libpng16.so* | */libfreetype.so* | */libbz2.so* | */libbrotlidec.so* | */libbrotlicommon.so*)
            name=$(basename "$lib")
            cp -L "$lib" "$app/usr/lib/$name"
            chmod 644 "$app/usr/lib/$name"
            objcopy --strip-debug "$app/usr/lib/$name" 2>/dev/null || true
            patchelf --set-rpath '$ORIGIN' "$app/usr/lib/$name" 2>/dev/null || true
            echo "bundled $name"
            ;;
    esac
done
if ldd "$app/usr/bin/MetroidOtherM" | grep -q 'not found'; then
    ldd "$app/usr/bin/MetroidOtherM" | grep 'not found' >&2
    echo "unresolved libraries" >&2
    exit 1
fi

# The program's own files beside it (copied there by the build: runtime/cmake/PublicProducts.cmake).
for f in dsp_coef.bin initial_pipeline_cache.db cacert.pem; do
    [[ -f $build/$f ]] && cp "$build/$f" "$app/usr/bin/"
done
cp -r "$build/wii_bootstrap" "$build/startup" "$app/usr/bin/"
cp "$nodtool" "$app/usr/bin/nodtool"
echo "$version" > "$app/usr/bin/version.txt"   # the updater compares this with the latest release's tag
commit=$(git -C "$root" rev-parse --short HEAD)
dirty=$(git -C "$root" -c core.autocrlf=true -c core.filemode=false status --porcelain --untracked-files=no | grep -q . && echo "+changes" || true)
printf '{\n  "SetupVersion": "Other M Recompiled %s (%s%s, built %s, Linux x86_64 AppImage)"\n}\n' \
    "$version" "$commit" "$dirty" "$(date -u +%Y-%m-%d)" > "$app/usr/bin/build-fingerprint.json"

# Licenses: the port (GPLv3), the components in the program, the dependency trees, Dawn, nodtool.
lic=$app/usr/share/otherm-recomp/licenses
cp "$root/LICENSE" "$app/usr/share/otherm-recomp/LICENSE.txt"
cp "$root/THIRD-PARTY-NOTICES.md" "$lic/"
for dir in "$root"/dependencies/*/; do
    name=$(basename "$dir")
    [[ $name == dawn_prebuilt || $name == cppwinrt ]] && continue
    for f in "$dir"LICEN[CS]E* "$dir"COPYING* "$dir"NOTICE*; do
        [[ -f $f ]] && cp "$f" "$lic/$name-$(basename "${f%.*}").txt"
    done
done
# Dawn's prebuilt package carries no license file: its BSD-3-Clause text from the source (fetched once).
[[ -f $work/dawn-LICENSE ]] || curl -fL -o "$work/dawn-LICENSE" https://raw.githubusercontent.com/google/dawn/main/LICENSE
cp "$work/dawn-LICENSE" "$lic/dawn-LICENSE.txt"
cp "$build/_deps/otherm_mbedtls_upstream-src/LICENSE" "$lic/mbedtls-LICENSE.txt"
# The system libraries bundled above: their Debian copyright files.
for f in /usr/share/doc/libpng16-16*/copyright /usr/share/doc/libfreetype6/copyright /usr/share/doc/libbz2-1.0/copyright \
         /usr/share/doc/libbrotli1/copyright; do
    [[ -f $f ]] && cp "$f" "$lic/system-$(basename "$(dirname "$f")")-copyright.txt"
done
cp "$work/nod-LICENSE-MIT" "$lic/nodtool-LICENSE-MIT.txt"
cp "$work/nod-LICENSE-APACHE" "$lic/nodtool-LICENSE-APACHE.txt"

cat > "$app/AppRun" <<'EOF'
#!/bin/sh
# Other M Recompiled. Put your own Metroid: Other M disc image (USA, R3OE01, revision 1: .rvz, .iso, .wia, .gcz,
# .ciso or .wbfs) next to this AppImage: the first start sets the game up from it into Disc/DATA there.
# Saves, settings and logs go to UserData/ beside the AppImage.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/MetroidOtherM" "$@"
EOF
chmod 755 "$app/AppRun"

cat > "$app/usr/share/applications/otherm-recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Other M Recompiled
GenericName=Metroid: Other M
Comment=Metroid: Other M recompiled for PC (your own disc image)
Exec=MetroidOtherM
Icon=otherm-recomp
Categories=Game;ActionGame;
Terminal=false
EOF
cp "$app/usr/share/applications/otherm-recomp.desktop" "$app/otherm-recomp.desktop"
cp "$root/runtime/assets/icon/otherm.png" "$app/usr/share/icons/hicolor/512x512/apps/otherm-recomp.png"
cp "$root/runtime/assets/icon/otherm.png" "$app/otherm-recomp.png"
ln -s otherm-recomp.png "$app/.DirIcon"
cp "$root/recomplauncher.json" "$app/recomplauncher.json"   # RecompLauncher reads /recomplauncher.json in the image

# Files copied off /mnt come out executable: only the programs should be.
find "$app" -type f -exec chmod 644 {} +
chmod 755 "$app/AppRun" "$app/usr/bin/MetroidOtherM" "$app/usr/bin/nodtool"
# Guard: no disc data in the download.
if find "$app" \( -type d \( -name Disc -o -name DATA -o -name sys -o -name files \) \) -o \
        \( -type f \( -iname '*.rvz' -o -iname '*.iso' -o -iname '*.wbfs' -o -iname '*.dol' -o -iname '*.rel' \) \) | grep -q .; then
    echo "disc data in the AppImage" >&2
    exit 1
fi

mkdir -p "$root/dist"
out=$root/dist/OtherMRecompiled-$version-x86_64.AppImage
sq=$work/AppDir.squashfs
rm -f "$sq" "$out"
mksquashfs "$app" "$sq" -root-owned -noappend -comp zstd -Xcompression-level 19 -b 1M -quiet
cat "$runtime" "$sq" > "$out"
chmod 755 "$out"
rm -f "$sq"
(cd "$root/dist" && sha256sum "$(basename "$out")" > SHA256SUMS.txt)
echo "DONE: $out ($(du -h "$out" | cut -f1))"
cat "$root/dist/SHA256SUMS.txt"

#!/usr/bin/env bash
# Packs the macOS build (tools/build-macos.sh) into an app, zipped for the release:
#   bash tools/package-macos.sh --version v1.0.0
# Output: dist/OtherMRecompiled-<version>-macos-arm64.zip (the name the in-game updater fetches; it holds one app,
# "Other M Recompiled.app") and its line in dist/SHA256SUMS.txt.
# The app holds the program (the game's code in recompiled form, for the player whose disc it was built from) and the
# port's own files, never the disc's. The program is in Contents/MacOS, its own files in Contents/Resources (where the
# runtime looks for them in an app: runtime_config.h ExecutableDirectory), dylibs it needs that macOS lacks in
# Contents/Frameworks. The player's files (UserData's, Metroid.ini, and the Disc/DATA the start screen sets up from
# their disc image) go to ~/Library/Application Support/OtherMRecompiled, never into the app.
# Ad-hoc signed (not notarized): the first start needs System Settings > Privacy & Security > Open Anyway.
# Downloads (once, into ~/otherm-macos): nodtool (github.com/encounter/nod, MIT / Apache-2.0; reads .rvz/.wia/.gcz).
# Written for the bash 3.2 macOS ships.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$HOME/otherm-macos
build=$work/build
version=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done
[[ $version =~ ^v[0-9]+\.[0-9]+\.[0-9]+[a-z]?$ ]] || { echo "--version vX.Y.Z is required" >&2; exit 1; }
exe=$build/MetroidOtherM
[[ -x $exe ]] || { echo "no $exe: run tools/build-macos.sh first" >&2; exit 1; }

nod_version=v2.0.0-alpha.10   # the same pin as tools/build-otherm.ps1 and package-linux.sh
nodtool=$work/nodtool-macos-arm64-$nod_version
if [[ ! -x $nodtool ]]; then
    curl -fL -o "$nodtool.tmp" "https://github.com/encounter/nod/releases/download/$nod_version/nodtool-macos-arm64"
    curl -fL -o "$work/nod-LICENSE-MIT" "https://raw.githubusercontent.com/encounter/nod/$nod_version/LICENSE-MIT"
    curl -fL -o "$work/nod-LICENSE-APACHE" "https://raw.githubusercontent.com/encounter/nod/$nod_version/LICENSE-APACHE"
    echo "e23ca466999b720c55e6d29c9683fce8cc74451ba64ead2e543d50129f24528a  $nodtool.tmp" | shasum -a 256 -c --quiet ||
        { rm -f "$nodtool.tmp"; echo "nodtool did not match its pinned hash" >&2; exit 1; }
    chmod 755 "$nodtool.tmp" && mv "$nodtool.tmp" "$nodtool"
fi

name="Other M Recompiled"
app=$work/$name.app
rm -rf "$app"
contents=$app/Contents
mkdir -p "$contents/MacOS" "$contents/Resources/licenses" "$contents/Frameworks"

cp "$exe" "$contents/MacOS/MetroidOtherM"
strip -S "$contents/MacOS/MetroidOtherM"   # debug info only: the symbols crash reports name functions by stay

# Dylibs the program loads from outside macOS itself (the build's own, Homebrew's): into Frameworks, found through
# @rpath. A system library (/usr/lib, /System) is left as it is.
bundle_dylibs() {
    local macho=$1 lib base
    otool -L "$macho" | tail -n +2 | awk '{print $1}' | while IFS= read -r lib; do
        case "$lib" in
            /usr/lib/* | /System/* | @rpath/* | @executable_path/* | @loader_path/*) continue ;;
        esac
        base=$(basename "$lib")
        if [[ ! -f $contents/Frameworks/$base ]]; then
            cp -L "$lib" "$contents/Frameworks/$base"
            chmod 644 "$contents/Frameworks/$base"
            install_name_tool -id "@rpath/$base" "$contents/Frameworks/$base" 2>/dev/null
            echo "bundled $base"
            bundle_dylibs "$contents/Frameworks/$base"
        fi
        install_name_tool -change "$lib" "@rpath/$base" "$macho" 2>/dev/null
    done
}
bundle_dylibs "$contents/MacOS/MetroidOtherM"
# @rpath dylibs the build made (shared SDL3 or Dawn): from the build folder.
{ otool -L "$contents/MacOS/MetroidOtherM" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' || true; } | while IFS= read -r lib; do
    base=${lib#@rpath/}
    [[ -f $contents/Frameworks/$base ]] && continue
    found=$(find "$build" -name "$base" -not -path '*/CMakeFiles/*' | head -n 1)
    [[ -n $found ]] || { echo "can't find $lib for the app" >&2; exit 1; }
    cp -L "$found" "$contents/Frameworks/$base"
    chmod 644 "$contents/Frameworks/$base"
    echo "bundled $base"
done
# The build's own search paths (its _deps folders, under the builder's home) go: only the app's Frameworks stays.
otool -l "$contents/MacOS/MetroidOtherM" | awk '/cmd LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}' |
    while IFS= read -r rpath; do
        [[ $rpath == @executable_path/../Frameworks ]] || install_name_tool -delete_rpath "$rpath" "$contents/MacOS/MetroidOtherM"
    done
if ! otool -l "$contents/MacOS/MetroidOtherM" | grep -A2 LC_RPATH | grep -q '@executable_path/../Frameworks'; then
    install_name_tool -add_rpath @executable_path/../Frameworks "$contents/MacOS/MetroidOtherM"
fi
rmdir "$contents/Frameworks" 2>/dev/null || true

# The program's own files (copied beside it by the build: runtime/cmake/PublicProducts.cmake).
res=$contents/Resources
for f in dsp_coef.bin initial_pipeline_cache.db cacert.pem; do
    [[ -f $build/$f ]] && cp "$build/$f" "$res/"
done
cp -R "$build/wii_bootstrap" "$build/startup" "$res/"
cp "$nodtool" "$res/nodtool"
echo "$version" > "$res/version.txt"   # the updater compares this with the latest release's tag
commit=$(git -C "$root" rev-parse --short HEAD)
dirty=$(git -C "$root" status --porcelain --untracked-files=no | grep -q . && echo "+changes" || true)
printf '{\n  "SetupVersion": "Other M Recompiled %s (%s%s, built %s, macOS arm64)"\n}\n' \
    "$version" "$commit" "$dirty" "$(date -u +%Y-%m-%d)" > "$res/build-fingerprint.json"

# The icon, from the 512x512 PNG.
iconset=$work/otherm.iconset
rm -rf "$iconset"
mkdir -p "$iconset"
png=$root/runtime/assets/icon/otherm.png
for size in 16 32 128 256; do
    sips -z $size $size "$png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) "$png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
cp "$png" "$iconset/icon_512x512.png"
iconutil -c icns "$iconset" -o "$res/otherm.icns"
rm -rf "$iconset"

short=${version#v}
short=${short%%[a-z]*}
cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>MetroidOtherM</string>
    <key>CFBundleIconFile</key><string>otherm</string>
    <key>CFBundleIdentifier</key><string>io.github.othermrecomp</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundleDisplayName</key><string>$name</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$short</string>
    <key>CFBundleVersion</key><string>$short</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    <key>NSBluetoothAlwaysUsageDescription</key><string>Wii Remotes connect over Bluetooth.</string>
    <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
EOF
plutil -lint "$contents/Info.plist" >/dev/null

# Licenses: the port (GPLv3), the components in the program, the dependency trees, Dawn, nodtool.
lic=$res/licenses
cp "$root/LICENSE" "$res/LICENSE.txt"
cp "$root/THIRD-PARTY-NOTICES.md" "$lic/"
for dir in "$root"/dependencies/*/; do
    dep=$(basename "$dir")
    [[ $dep == dawn_prebuilt || $dep == cppwinrt ]] && continue
    for f in "$dir"LICEN[CS]E* "$dir"COPYING* "$dir"NOTICE*; do
        [[ -f $f ]] && cp "$f" "$lic/$dep-$(basename "${f%.*}").txt"
    done
done
# Dawn's prebuilt package carries no license file: its BSD-3-Clause text from the source (fetched once).
[[ -f $work/dawn-LICENSE ]] || curl -fL -o "$work/dawn-LICENSE" https://raw.githubusercontent.com/google/dawn/main/LICENSE
cp "$work/dawn-LICENSE" "$lic/dawn-LICENSE.txt"
[[ -f $build/_deps/otherm_mbedtls_upstream-src/LICENSE ]] && cp "$build/_deps/otherm_mbedtls_upstream-src/LICENSE" "$lic/mbedtls-LICENSE.txt"
cp "$work/nod-LICENSE-MIT" "$lic/nodtool-LICENSE-MIT.txt"
cp "$work/nod-LICENSE-APACHE" "$lic/nodtool-LICENSE-APACHE.txt"

find "$app" -type f -exec chmod 644 {} +
chmod 755 "$contents/MacOS/MetroidOtherM" "$res/nodtool"
# Guard: no disc data in the download.
if find "$app" \( -type d \( -name Disc -o -name DATA -o -name sys -o -name files \) \) -o \
        \( -type f \( -iname '*.rvz' -o -iname '*.iso' -o -iname '*.wbfs' -o -iname '*.dol' -o -iname '*.rel' \) \) | grep -q .; then
    echo "disc data in the app" >&2
    exit 1
fi
# Guard: the builder's home folder anywhere in the app (build-macos.sh maps source paths away, the rpaths are
# rewritten above). Raw bytes, not strings(1): that misses Mach-O load commands.
leaks=$(find "$app" -type f -exec env LC_ALL=C grep -laF "$HOME/" {} + || true)
if [[ -n $leaks ]]; then
    echo "$leaks" >&2
    echo "the builder's home folder is in the app" >&2
    exit 1
fi

# Ad-hoc signature: every Mach-O inside first, then the app.
find "$contents" -type f -not -path "$contents/MacOS/*" | while IFS= read -r f; do
    if file -b "$f" | grep -q Mach-O; then codesign --force --sign - "$f"; fi
done
codesign --force --sign - "$app"
codesign --verify --strict --deep "$app"

mkdir -p "$root/dist"
out=$root/dist/OtherMRecompiled-$version-macos-arm64.zip
rm -f "$out"
(cd "$work" && ditto -c -k --keepParent --norsrc "$name.app" "$out")
(cd "$root/dist" && shasum -a 256 "$(basename "$out")" > SHA256SUMS.txt)
echo "DONE: $out ($(du -h "$out" | cut -f1))"
cat "$root/dist/SHA256SUMS.txt"

# Android (arm64-v8a) build of Other M Recompiled: libmain.so from the translation tools\build-otherm.ps1 made, then a
# signed APK, without Gradle: the NDK's CMake toolchain compiles the game (SDL and Dawn linked in), the SDK's build
# tools pack it (javac + d8 for SDL's Java and the port's activity, aapt2 for the manifest, icons and assets, zipalign,
# apksigner).
#   powershell -ExecutionPolicy Bypass -File tools\build-android.ps1 [-Version v1.0.0] [-Jobs N] [-Configure]
#       [-SkipNative] [-Env "NAME=value;..."] [-Out <apk>] [-Keystore <jks>]
# Needs: the Android SDK (ANDROID_HOME, else %LOCALAPPDATA%\Android\Sdk) with an NDK (r27 or newer), build-tools and
# platforms;android-35; a JDK 17+ (JAVA_HOME, else Android Studio's); Rust (rustup: the script adds its
# aarch64-linux-android target); Python 3; the portable CMake and Ninja in toolchain\ (tools\fetch-tools.ps1).
# nod (github.com/encounter/nod, MIT / Apache-2.0) reads the player's disc image inside the app, every format, through
# the file the system's picker hands over (an app can't open shared storage by path): a pinned release, built once into
# toolchain\android\.
# Signing: -Keystore, its password in <keystore without .jks>.password.txt; by default
# %USERPROFILE%\.android\otherm-release.jks, created on first use. Keep both files: Android installs an update only
# when it is signed with the same key.
# -Env "OTHERM_SLOW_FRAMES_ALL=1;...": environment variables for the app (SDL's SDL_ENV.* meta-data; debugging).
# Output: dist\OtherMRecompiled-<version>-android-arm64.apk. Logs: build-logs\android-*.log.
[CmdletBinding()]
param(
    [string]$Version = 'v0.0.0',
    [int]$VersionCode = 0,
    [int]$Jobs = 0,
    [string]$Out = '',
    [string]$Keystore = '',
    [string]$Env = '',
    [switch]$Configure,
    [switch]$SkipNative
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $PSScriptRoot
$toolchain = Join-Path $root 'toolchain'
$dependencies = Join-Path $root 'dependencies'
$logs = Join-Path $root 'build-logs'
$build = Join-Path $root 'build-android'
$minSdk = 30      # Android 11: guest memory is a memfd (guest_flat_memory.cpp), which bionic has from API 30
$targetSdk = 35
if ($Version -notmatch '^v(\d+)\.(\d+)\.(\d+)$') { throw "-Version is v<major>.<minor>.<patch>, not $Version" }
$versionName = $Version.Substring(1)
if ($VersionCode -le 0) { $VersionCode = [Math]::Max(1, [int]$Matches[1] * 10000 + [int]$Matches[2] * 100 + [int]$Matches[3]) }
if (-not $Out) { $Out = Join-Path $root "dist\OtherMRecompiled-$Version-android-arm64.apk" }
if ($Jobs -le 0) {
    # A translated shard compile can take ~2.5 GB (tools\build-otherm.ps1).
    $ramGb = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB
    $Jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [int][Math]::Floor($ramGb / 2.5)))
}
[IO.Directory]::CreateDirectory($logs) | Out-Null

function ConvertTo-CMakePath([string]$Path) { return [IO.Path]::GetFullPath($Path).Replace('\', '/') }
function Newest([string]$Directory) {
    Get-ChildItem -LiteralPath $Directory -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)*$' } |
        Sort-Object { [version]($_.Name + $(if ($_.Name -notmatch '\.') { '.0' } else { '' })) } |
        Select-Object -Last 1
}
function Set-Status([string]$Text) { Write-Host ("{0:HH:mm:ss} {1}" -f (Get-Date), $Text) }
# Runs a native tool, keeps its output in build-logs\android-<name>.log and stops on a non-zero exit code.
function Invoke-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$WorkDir = $root) {
    $log = Join-Path $logs "android-$Name.log"
    Set-Status "${Name}: started"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'            # Windows PowerShell 5.1 turns native stderr into errors
    Push-Location $WorkDir
    try { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $log -Encoding utf8 }
    finally { Pop-Location; $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $log -Tail 40
        throw "$Name failed with exit code $LASTEXITCODE (see $log)"
    }
    Set-Status "${Name}: ok"
}
function Get-Download([string]$Url, [string]$File, [string]$Sha256) {
    $downloads = Join-Path $toolchain 'downloads'
    [IO.Directory]::CreateDirectory($downloads) | Out-Null
    $path = Join-Path $downloads $File
    if (-not (Test-Path -LiteralPath $path)) {
        Set-Status "downloading $File"
        & curl.exe -fsSL -o "$path.tmp" $Url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
        Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ine $Sha256) {
        Remove-Item -LiteralPath $path
        throw "$File did not match its pinned hash"
    }
    return $path
}

# --- tools --------------------------------------------------------------------------------------------------------
$sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $sdk) { throw 'Android SDK not found (set ANDROID_HOME)' }
$ndk = if ($env:ANDROID_NDK_ROOT -and (Test-Path $env:ANDROID_NDK_ROOT)) { $env:ANDROID_NDK_ROOT } else { (Newest "$sdk\ndk").FullName }
if (-not $ndk) { throw "no NDK in $sdk\ndk (sdkmanager `"ndk;27.3.13750724`")" }
$ndkBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$bt = (Newest "$sdk\build-tools").FullName
if (-not $bt) { throw "no build-tools in $sdk" }
$androidJar = "$sdk\platforms\android-$targetSdk\android.jar"
if (-not (Test-Path $androidJar)) { throw "missing $androidJar (sdkmanager `"platforms;android-$targetSdk`")" }
$jdk = @($env:JAVA_HOME, "$env:ProgramFiles\Android\Android Studio\jbr") | Where-Object { $_ -and (Test-Path "$_\bin\javac.exe") } | Select-Object -First 1
if (-not $jdk) { throw 'no JDK (set JAVA_HOME)' }
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'python not found' }
$cmake = Join-Path $toolchain 'CMake\bin\cmake.exe'
$ninja = Join-Path $toolchain 'Ninja\ninja.exe'
foreach ($tool in $cmake, $ninja) { if (-not (Test-Path -LiteralPath $tool)) { throw "missing $tool (run tools\build-otherm.ps1 once)" } }
Set-Status "NDK $ndk; build-tools $bt; JDK $jdk; $Jobs jobs"

# Paths the compilers record (__FILE__, panic messages) are mapped to neutral names, so no folder of this PC ships.
$sdkMap = ConvertTo-CMakePath $sdk
$rootMap = ConvertTo-CMakePath $root
$prefixMaps = "-ffile-prefix-map=$sdkMap=/android-sdk -ffile-prefix-map=$rootMap=/otherm"

# --- nod's C library (libnod.so) -----------------------------------------------------------------------------------
# Its own shared library: Rust exports only the nod_* functions, so the compression code inside (zstd, lzma, bzip2)
# never meets the runtime's own zstd. Android's libz.so is a system library.
$nodVersion = 'v2.0.0-alpha.12'
$nodDir = Join-Path $toolchain "android\libnod-$nodVersion"
if (-not (Test-Path -LiteralPath (Join-Path $nodDir 'lib\libnod.so'))) {
    $cargo = Get-Command cargo -ErrorAction SilentlyContinue
    $rustup = Get-Command rustup -ErrorAction SilentlyContinue
    if (-not $cargo -or -not $rustup) { throw 'Rust (cargo, rustup) not found: https://rustup.rs' }
    Invoke-Step 'rust-target' $rustup.Source @('target', 'add', 'aarch64-linux-android')
    $archive = Get-Download "https://github.com/encounter/nod/archive/refs/tags/$nodVersion.tar.gz" "nod-$nodVersion.tar.gz" `
        'e696c90d37cbf9f1daf9edafca021c2a4006a9b9d7261af6f573f602522aa5cc'
    $nodSrcRoot = Join-Path $toolchain 'android\nod-src'
    if (Test-Path -LiteralPath $nodSrcRoot) { Remove-Item -LiteralPath $nodSrcRoot -Recurse -Force }
    [IO.Directory]::CreateDirectory($nodSrcRoot) | Out-Null
    & tar.exe -xzf $archive -C $nodSrcRoot
    if ($LASTEXITCODE -ne 0) { throw "could not unpack $archive" }
    $nodSrc = (Get-ChildItem -LiteralPath $nodSrcRoot -Directory | Select-Object -First 1).FullName
    $cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE '.cargo' }
    $clang = Join-Path $ndkBin "aarch64-linux-android$minSdk-clang.cmd"
    $env:CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER = $clang
    $env:CC_aarch64_linux_android = $clang
    $env:AR_aarch64_linux_android = Join-Path $ndkBin 'llvm-ar.exe'
    $env:CFLAGS_aarch64_linux_android = "-ffile-prefix-map=$(ConvertTo-CMakePath $cargoHome)=/cargo -ffile-prefix-map=$(ConvertTo-CMakePath $nodSrc)=/nod"
    # (a soname, so libmain.so asks for "libnod.so" and not the path it was linked from)
    $env:CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS = "-C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wl,-soname,libnod.so " +
        "--remap-path-prefix=$cargoHome=/cargo --remap-path-prefix=$nodSrc=/nod"
    $env:CARGO_TARGET_DIR = Join-Path $toolchain 'android\nod-target'
    Invoke-Step 'libnod' $cargo.Source @('build', '--release', '--locked', '--target', 'aarch64-linux-android', '-p', 'nod-ffi',
        '--no-default-features', '--features', 'compress-bzip2-static compress-lzma-static compress-zlib-static compress-zstd-static threading') $nodSrc
    foreach ($d in 'lib', 'include', 'licenses') { [IO.Directory]::CreateDirectory((Join-Path $nodDir $d)) | Out-Null }
    Copy-Item -LiteralPath (Join-Path $env:CARGO_TARGET_DIR 'aarch64-linux-android\release\libnod.so') -Destination (Join-Path $nodDir 'lib\libnod.so') -Force
    Copy-Item -LiteralPath (Join-Path $nodSrc 'nod-ffi\include\nod.h') -Destination (Join-Path $nodDir 'include\nod.h') -Force
    # The licenses of what libnod.so holds: nod, and the C libraries its crates compile in (bzip2, xz's liblzma, zstd),
    # at the versions Cargo.lock pins.
    $licenses = Join-Path $nodDir 'licenses'
    foreach ($license in 'LICENSE-MIT', 'LICENSE-APACHE') {
        Copy-Item -LiteralPath (Join-Path $nodSrc $license) -Destination (Join-Path $licenses "nod-$license.txt") -Force
    }
    $lock = Get-Content -Raw -LiteralPath (Join-Path $nodSrc 'Cargo.lock')
    foreach ($entry in @(@('bzip2-sys', 'bzip2-*\LICENSE', 'bzip2-LICENSE.txt'),
                         @('liblzma-sys', 'xz\COPYING', 'xz-COPYING.txt'),
                         @('liblzma-sys', 'xz\COPYING.0BSD', 'xz-COPYING.0BSD.txt'),
                         @('zstd-sys', 'zstd\LICENSE', 'zstd-LICENSE.txt'))) {
        $crate, $file, $name = $entry
        if ($lock -notmatch "name = `"$crate`"\r?\nversion = `"([^`"]+)`"") { throw "Cargo.lock has no $crate" }
        $crateDir = Get-Item -Path (Join-Path $cargoHome "registry\src\*\$crate-$($Matches[1])") | Select-Object -First 1
        if (-not $crateDir) { throw "no $crate $($Matches[1]) in $cargoHome\registry" }
        $source = Get-ChildItem -Path (Join-Path $crateDir.FullName $file) -File | Select-Object -First 1
        if (-not $source) { throw "no $file in $($crateDir.FullName)" }
        Copy-Item -LiteralPath $source.FullName -Destination (Join-Path $licenses $name) -Force
    }
    foreach ($name in 'CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER', 'CC_aarch64_linux_android', 'AR_aarch64_linux_android',
                      'CFLAGS_aarch64_linux_android', 'CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS', 'CARGO_TARGET_DIR') {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
}

# --- libmain.so ----------------------------------------------------------------------------------------------------
if (-not $SkipNative) {
    if (-not (Test-Path -LiteralPath (Join-Path $root 'generated\build_shards\shards.cmake'))) {
        throw 'no translated game: run tools\build-otherm.ps1 first'
    }
    if ($Configure -or -not (Test-Path -LiteralPath (Join-Path $build 'build.ninja'))) {
        $arguments = @(
            '-S', (ConvertTo-CMakePath (Join-Path $root 'runtime')), '-B', (ConvertTo-CMakePath $build), '-G', 'Ninja',
            "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $ninja)",
            "-DCMAKE_TOOLCHAIN_FILE=$(ConvertTo-CMakePath $ndk)/build/cmake/android.toolchain.cmake",
            '-DANDROID_ABI=arm64-v8a', "-DANDROID_PLATFORM=android-$minSdk", '-DANDROID_STL=c++_static',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON',
            # aurora's third-party libraries (libpng, zlib) inside libmain.so: an app may only need Android's own.
            '-DBUILD_SHARED_LIBS=OFF',
            '-DAURORA_DAWN_PROVIDER=package', '-DAURORA_SDL3_PROVIDER=vendor', '-DAURORA_SDL3_LINKAGE=static',
            '-DCMAKE_POLICY_DEFAULT_CMP0168=NEW',
            "-DCMAKE_C_FLAGS=$prefixMaps", "-DCMAKE_CXX_FLAGS=$prefixMaps", "-DCMAKE_ASM_FLAGS=$prefixMaps",
            "-DOTHERM_TRANSLATED_COMPILE_JOBS=$Jobs", "-DOTHERM_LIBNOD_DIR=$(ConvertTo-CMakePath $nodDir)")
        # The pinned dependency sources; Android's Dawn package is fetched by aurora (hash-pinned), and the Windows-only
        # Dawn, C++/WinRT and libusb trees stay out.
        foreach ($directory in Get-ChildItem -LiteralPath $dependencies -Directory) {
            if ($directory.Name -in 'dawn_prebuilt', 'cppwinrt', 'libusb') { continue }
            $arguments += "-DFETCHCONTENT_SOURCE_DIR_$($directory.Name.ToUpperInvariant())=$(ConvertTo-CMakePath $directory.FullName)"
        }
        Invoke-Step 'configure' $cmake $arguments
    }
    Invoke-Step 'compile' $cmake @('--build', $build, '--target', 'OtherMRecompiled', '--parallel', "$Jobs")
}
$so = Join-Path $build 'libmain.so'
if (-not (Test-Path -LiteralPath $so)) { throw "no $so" }

# --- APK -----------------------------------------------------------------------------------------------------------
$stage = Join-Path $build 'apk'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($d in 'classes', 'dex', 'lib\arm64-v8a', 'assets\runtime') { [IO.Directory]::CreateDirectory((Join-Path $stage $d)) | Out-Null }

# Native code without its debug information (the symbol table stays, for crash stacks; build-android\libmain.so is the
# one to symbolize with).
$strip = Join-Path $ndkBin 'llvm-strip.exe'
Invoke-Step 'strip' $strip @('--strip-debug', '-o', (Join-Path $stage 'lib\arm64-v8a\libmain.so'), $so)
Copy-Item -LiteralPath (Join-Path $nodDir 'lib\libnod.so') -Destination (Join-Path $stage 'lib\arm64-v8a\libnod.so')

# The files a desktop build keeps beside the program (OtherMActivity unpacks them into the app's runtime folder), the
# version the updater compares, the default controller profile, the licenses.
$assets = Join-Path $stage 'assets'
$runtimeAssets = Join-Path $assets 'runtime'
foreach ($f in 'dsp_coef.bin', 'initial_pipeline_cache.db', 'cacert.pem') {
    Copy-Item -LiteralPath (Join-Path $build $f) -Destination (Join-Path $runtimeAssets $f)
}
foreach ($d in 'wii_bootstrap', 'startup') { Copy-Item -LiteralPath (Join-Path $build $d) -Destination (Join-Path $runtimeAssets $d) -Recurse }
Set-Content -LiteralPath (Join-Path $runtimeAssets 'version.txt') -Encoding ascii -Value $Version   # the updater compares it
# The log's first line (as tools\package-windows.ps1 writes it).
$commit = (& git -C $root rev-parse --short HEAD).Trim()
$dirty = if ((& git -C $root -c core.autocrlf=true -c core.filemode=false status --porcelain --untracked-files=no)) { '+changes' } else { '' }
$built = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
Set-Content -LiteralPath (Join-Path $runtimeAssets 'build-fingerprint.json') -Encoding ascii -Value @(
    '{', "  `"SetupVersion`": `"Other M Recompiled $Version ($commit$dirty, built $built, Android arm64 APK)`"", '}')
Copy-Item -LiteralPath (Join-Path $root 'project\controls\Metroid.ini') -Destination (Join-Path $assets 'Metroid.ini')
# Licenses (as tools\package-windows.ps1): the port (GPLv3), the components in the program, the dependency trees, Dawn,
# Mbed TLS (the TLS of every system but Windows), and nod with what it compiles in.
$licenses = Join-Path $runtimeAssets 'licenses'
[IO.Directory]::CreateDirectory($licenses) | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $licenses 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination (Join-Path $licenses 'THIRD-PARTY-NOTICES.md')
Get-ChildItem -LiteralPath $dependencies -Directory | Where-Object { $_.Name -notin 'dawn_prebuilt', 'cppwinrt', 'libusb' } | ForEach-Object {
    $name = $_.Name
    Get-ChildItem -LiteralPath $_.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenses "$name-$($_.BaseName).txt") }
}
$dawnLicense = Join-Path $toolchain 'downloads\dawn-LICENSE'
if (-not (Test-Path -LiteralPath $dawnLicense)) {
    & curl.exe -fsSL -o $dawnLicense 'https://raw.githubusercontent.com/google/dawn/main/LICENSE'
    if ($LASTEXITCODE -ne 0) { throw 'could not download the Dawn license' }
}
Copy-Item -LiteralPath $dawnLicense -Destination (Join-Path $licenses 'dawn-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $build '_deps\otherm_mbedtls_upstream-src\LICENSE') -Destination (Join-Path $licenses 'mbedtls-LICENSE.txt')
Get-ChildItem -LiteralPath (Join-Path $nodDir 'licenses') -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $licenses }
# What the activity compares to decide whether to unpack again: this build's library and assets.
$stamp = (Get-FileHash -Algorithm SHA256 -LiteralPath $so).Hash.Substring(0, 16) + "-$Version"
Set-Content -LiteralPath (Join-Path $runtimeAssets 'build.txt') -Encoding ascii -NoNewline -Value $stamp

# Java: SDL's activity (the copy matching the SDL built in) and the port's.
$sdlJava = Join-Path $dependencies 'SDL\android-project\app\src\main\java'
$sources = @(Get-ChildItem -LiteralPath $sdlJava, (Join-Path $root 'android\java') -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$sources | Set-Content -Encoding ascii (Join-Path $stage 'sources.txt')
Invoke-Step 'javac' (Join-Path $jdk 'bin\javac.exe') @('-source', '11', '-target', '11', '-Xlint:-options', '-nowarn', '-encoding', 'UTF-8',
    '-classpath', $androidJar, '-d', (Join-Path $stage 'classes'), "@$(Join-Path $stage 'sources.txt')")
Invoke-Step 'jar' (Join-Path $jdk 'bin\jar.exe') @('cf', (Join-Path $stage 'classes.jar'), '-C', (Join-Path $stage 'classes'), '.')
Invoke-Step 'd8' (Join-Path $bt 'd8.bat') @('--release', '--min-api', "$minSdk", '--lib', $androidJar, '--output', (Join-Path $stage 'dex'),
    (Join-Path $stage 'classes.jar'))

# Manifest (-Env: environment variables SDL sets before the game starts), resources, assets.
$manifest = Join-Path $root 'android\AndroidManifest.xml'
if ($Env) {
    $meta = ($Env -split ';' | Where-Object { $_ -match '^([^=]+)=(.*)$' } | ForEach-Object {
        "        <meta-data android:name=`"SDL_ENV.$($Matches[1])`" android:value=`"$($Matches[2])`" />" }) -join "`n"
    (Get-Content -Raw -LiteralPath $manifest).Replace('    </application>', "$meta`n    </application>") |
        Set-Content -Encoding utf8 (Join-Path $stage 'AndroidManifest.xml')
    $manifest = Join-Path $stage 'AndroidManifest.xml'
    Set-Status "app environment: $Env"
}
Invoke-Step 'aapt2-compile' (Join-Path $bt 'aapt2.exe') @('compile', '--dir', (Join-Path $root 'android\res'), '-o', (Join-Path $stage 'res.zip'))
Invoke-Step 'aapt2-link' (Join-Path $bt 'aapt2.exe') @('link', '-o', (Join-Path $stage 'base.apk'), '-I', $androidJar, '--manifest', $manifest,
    '--min-sdk-version', "$minSdk", '--target-sdk-version', "$targetSdk", '--version-code', "$VersionCode",
    '--version-name', $versionName, '-A', $assets, (Join-Path $stage 'res.zip'))

# classes.dex and the libraries into it (Python's zipfile appends without touching aapt2's entries). The libraries are
# compressed (Android unpacks them at install, android:extractNativeLibs): libmain.so is 165 MB as it is, 58 MB
# compressed, and the download is what players and the updater wait for.
$zipScript = @'
import sys, zipfile
apk, dex = sys.argv[1:3]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    z.write(dex, "classes.dex")
    for so in sys.argv[3:]:
        z.write(so, "lib/arm64-v8a/" + so.replace("\\", "/").rsplit("/", 1)[1])
'@
$zipScript | Set-Content -Encoding ascii (Join-Path $stage 'addfiles.py')
Invoke-Step 'zip' $python @('-I', (Join-Path $stage 'addfiles.py'), (Join-Path $stage 'base.apk'), (Join-Path $stage 'dex\classes.dex'),
    (Join-Path $stage 'lib\arm64-v8a\libnod.so'), (Join-Path $stage 'lib\arm64-v8a\libmain.so'))
Invoke-Step 'zipalign' (Join-Path $bt 'zipalign.exe') @('-f', '-P', '16', '4', (Join-Path $stage 'base.apk'), (Join-Path $stage 'aligned.apk'))

# Signing.
if (-not $Keystore) { $Keystore = Join-Path $env:USERPROFILE '.android\otherm-release.jks' }
$passwordFile = [IO.Path]::ChangeExtension($Keystore, $null).TrimEnd('.') + '.password.txt'
if (-not (Test-Path -LiteralPath $Keystore)) {
    if (-not (Test-Path -LiteralPath $passwordFile)) {
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        [Convert]::ToBase64String($bytes).Replace('/', '_').Replace('+', '-') | Set-Content -NoNewline -Encoding ascii $passwordFile
    }
    Set-Status "creating the signing key $Keystore (password in $passwordFile; keep both)"
    Invoke-Step 'keytool' (Join-Path $jdk 'bin\keytool.exe') @('-genkeypair', '-keystore', $Keystore, '-alias', 'otherm', '-keyalg', 'RSA',
        '-keysize', '4096', '-validity', '36500', '-dname', 'CN=Other M Recompiled', '-storepass:file', $passwordFile, '-keypass:file', $passwordFile)
}
if (-not (Test-Path -LiteralPath $passwordFile)) { throw "no password file $passwordFile for $Keystore" }
[IO.Directory]::CreateDirectory((Split-Path -Parent $Out)) | Out-Null
# (the key's password is the store's: PKCS12, as keytool makes it)
Invoke-Step 'apksigner' (Join-Path $bt 'apksigner.bat') @('sign', '--ks', $Keystore, '--ks-key-alias', 'otherm', '--ks-pass', "file:$passwordFile",
    '--out', $Out, (Join-Path $stage 'aligned.apk'))
Invoke-Step 'apksigner-verify' (Join-Path $bt 'apksigner.bat') @('verify', $Out)
Set-Status ("{0}: {1:N0} MB (version {2}, code {3})" -f $Out, ((Get-Item -LiteralPath $Out).Length / 1MB), $versionName, $VersionCode)

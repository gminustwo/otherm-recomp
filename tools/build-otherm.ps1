# One-shot Metroid: Other M build: portable tools -> pinned dependencies -> translate -> configure -> compile.
# Run tools\setup-otherm.ps1 once first (disc, main.dol, pre-linked game.rel, function map).
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File <project>\tools\build-otherm.ps1
# Everything it downloads is pinned (tools\fetch-tools.ps1: llvm-mingw, CMake, Ninja into toolchain\;
# tools\fetch-dependencies.ps1: SDL3, Dawn and the other source trees into dependencies\); nothing else is
# installed. Logs: <project>\build-logs\.
#
# -TranslateOnly stops before the C++ build (the translator steps alone take ~30 s).
# -ShardsOnly reruns only the shard step after the translation (enough after editing the resumable function
# list, project/resumable_functions.inc, which the save states need).
# -PlayDir installs into another folder than Play\ (a test copy), closing only the game running from there.
[CmdletBinding()]
param(
    [int]$Jobs = 0,
    [switch]$SkipTools,
    [switch]$SkipTranslate,
    [switch]$TranslateOnly,
    [switch]$ConfigureOnly,
    [switch]$ShardsOnly,
    [string]$PlayDir = 'Play'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $PSScriptRoot            # <project>\tools -> <project>
$toolchain = Join-Path $root 'toolchain'
$dependencies = Join-Path $root 'dependencies'
$logs = Join-Path $root 'build-logs'
$project = 'project/recomp.yml'
$nativeDir = 'runtime/src'                           # the runtime's overrides, retargeted to Other M (project/retarget.tsv)
$workspace = $root      # the translator's cwd: runtime\, generated\ and build\ live under it
$module = 'engine_usa'
$playExeName = 'MetroidOtherM.exe'
if ($Jobs -le 0) {
    # A translated shard compile can take ~2.5 GB (measured on a translation this size); one job per core
    # exhausted a 16 GB machine there.
    $ramGb = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB
    $Jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [int][Math]::Floor($ramGb / 2.5)))
}
[IO.Directory]::CreateDirectory($logs) | Out-Null

# The build sees only the portable toolchain: an ambient Visual Studio or MSYS2 on PATH could pull in its own
# headers, libraries or linker.
function Get-ToolchainPath([string]$ToolchainRoot) {
    $full = [IO.Path]::GetFullPath($ToolchainRoot)
    return (@((Join-Path $full 'llvm-mingw\bin'), (Join-Path $full 'CMake\bin'), (Join-Path $full 'Ninja'),
              (Join-Path $env:SystemRoot 'System32'), $env:SystemRoot) -join ';')
}

function ConvertTo-CMakePath([string]$Path) { return [IO.Path]::GetFullPath($Path).Replace('\', '/') }

# The CMake configure command line. Each folder in dependencies\ is a pinned FetchContent source tree, mapped
# through FETCHCONTENT_SOURCE_DIR_<NAME> (FetchContent upper-cases the declared name and keeps its punctuation).
function Get-ConfigureArguments([string]$SourceDirectory, [string]$BuildDirectory, [string]$Bin, [string]$Ninja,
                                [string]$DependenciesDirectory, [string[]]$AdditionalArguments) {
    $arguments = @(
        '-S', (ConvertTo-CMakePath $SourceDirectory), '-B', (ConvertTo-CMakePath $BuildDirectory), '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $Ninja)",
        "-DCMAKE_C_COMPILER=$(ConvertTo-CMakePath (Join-Path $Bin 'x86_64-w64-mingw32-clang.exe'))",
        "-DCMAKE_CXX_COMPILER=$(ConvertTo-CMakePath (Join-Path $Bin 'x86_64-w64-mingw32-clang++.exe'))",
        "-DCMAKE_RC_COMPILER=$(ConvertTo-CMakePath (Join-Path $Bin 'x86_64-w64-mingw32-windres.exe'))",
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_SYSTEM_PROCESSOR=x86_64',
        '-DAURORA_DAWN_PROVIDER=package', '-DAURORA_SDL3_PROVIDER=vendor',
        '-DCMAKE_POLICY_DEFAULT_CMP0168=NEW', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DAWK:FILEPATH=')
    if (Test-Path -LiteralPath (Join-Path $DependenciesDirectory 'cppwinrt\winrt\base.h') -PathType Leaf) {
        $arguments += "-DOTHERM_CPPWINRT_INCLUDE_DIR=$(ConvertTo-CMakePath (Join-Path $DependenciesDirectory 'cppwinrt'))"
    }
    foreach ($directory in Get-ChildItem -LiteralPath $DependenciesDirectory -Directory) {
        $arguments += "-DFETCHCONTENT_SOURCE_DIR_$($directory.Name.ToUpperInvariant())=$(ConvertTo-CMakePath $directory.FullName)"
    }
    return $arguments + $AdditionalArguments
}

function Set-Status([string]$Text) {
    $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $Text
    Write-Host $line
    Add-Content -LiteralPath (Join-Path $logs 'status.txt') -Value $line
}

# Runs a native tool, keeps its output in build-logs\<name>.log and stops on a non-zero exit code.
function Invoke-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$WorkDir) {
    $log = Join-Path $logs "$Name.log"
    Set-Status "${Name}: started"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'            # Windows PowerShell 5.1 turns native stderr into errors
    Push-Location $WorkDir
    try { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $log -Encoding utf8 }
    finally { Pop-Location; $ErrorActionPreference = $previous }
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Set-Status "${Name}: FAILED (exit $code), see $log"
        Get-Content -LiteralPath $log -Tail 40
        throw "$Name failed with exit code $code."
    }
    Set-Status "${Name}: ok"
}

Remove-Item -LiteralPath (Join-Path $logs 'status.txt') -ErrorAction SilentlyContinue
Set-Status "build started, project $root, $Jobs jobs"

foreach ($required in @((Join-Path $root 'game\main.dol'), (Join-Path $root 'game\otherm_linked.rel'), (Join-Path $root $project),
                        (Join-Path $root 'project\MAP.txt'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing $required (run tools\setup-otherm.ps1 first)" }
}

# 1. Translate (.NET 8 SDK builds the translator; the runtime alone is enough to run it afterwards)
if (-not $SkipTranslate) {
    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnet) { throw 'dotnet was not found on PATH (the .NET 8 SDK).' }
    $env:DOTNET_ROLL_FORWARD = 'Major'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    Invoke-Step 'translator-build' $dotnet.Source @('build', 'translator\src\Translator.Cli\Translator.Cli.csproj', '-c', 'Release', '-nologo', '-v', 'q') $workspace
    $translator = Join-Path $root 'translator\src\Translator.Cli\bin\Release\net8.0\Translator.Cli.dll'
    if (-not $ShardsOnly) {
    Invoke-Step 'translate' $dotnet.Source @($translator, 'translate-recursive', '0x80006310', '--project', $project,
        '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json',
        '--production-source-bundle', 'generated/base_translation_sources.bin',
        '--no-function-files', '--prune-stale', '--threads', "$Jobs") $workspace
    Invoke-Step 'data-init' $dotnet.Source @($translator, 'generate-data-init', '--project', $project) $workspace
    }
    Invoke-Step 'shards' $dotnet.Source @($translator, 'emit-build-shards', '--project', $project,
        '--base-metadata', 'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions',
        '--native-source-dir', $nativeDir, '--resumable-functions', 'project/resumable_functions.inc',
        '--out', 'generated/build_shards') $workspace
}
if ($TranslateOnly) { Set-Status 'translate only: done'; exit 0 }

# 2. Toolchain and dependencies
if (-not $SkipTools) {
    if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe'))) {
        Set-Status 'tools: downloading llvm-mingw, CMake and Ninja'
        & (Join-Path $PSScriptRoot 'fetch-tools.ps1') -Destination $toolchain *>&1 |
            ForEach-Object { "$_" } | Out-File -LiteralPath (Join-Path $logs 'tools.log') -Encoding utf8
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dependencies 'dawn_prebuilt\bin\webgpu_dawn.dll'))) {
        Set-Status 'dependencies: downloading the pinned source trees and Dawn'
        & (Join-Path $PSScriptRoot 'fetch-dependencies.ps1') -Destination $dependencies *>&1 |
            ForEach-Object { "$_" } | Out-File -LiteralPath (Join-Path $logs 'dependencies.log') -Encoding utf8
    }
    foreach ($tool in @('llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe', 'CMake\bin\cmake.exe', 'Ninja\ninja.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $toolchain $tool))) { throw "Toolchain file missing after setup: $tool (see build-logs\tools.log)" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $dependencies 'dawn_prebuilt\bin\webgpu_dawn.dll'))) {
        throw 'Dependencies are incomplete (see build-logs\dependencies.log)'
    }
    Set-Status 'tools and dependencies ready'
}

# 3. Configure and compile with the portable toolchain on a scrubbed PATH
$env:PATH = Get-ToolchainPath $toolchain
$build = Join-Path $root 'build'
$configure = Get-ConfigureArguments (Join-Path $root 'runtime') $build (Join-Path $toolchain 'llvm-mingw\bin') `
    (Join-Path $toolchain 'Ninja\ninja.exe') $dependencies `
    @("-DOTHERM_TRANSLATED_COMPILE_JOBS=$Jobs", '-DOTHERM_ENGINE_MODULE=ON', "-DOTHERM_ENGINE_MODULE_NAME=$module")
$cmake = Join-Path $toolchain 'CMake\bin\cmake.exe'
Invoke-Step 'configure' $cmake $configure $workspace
if ($ConfigureOnly) { Set-Status 'configure only: done'; exit 0 }
Invoke-Step 'compile' $cmake @('--build', $build, '--target', 'OtherMRecompiled', 'OtherMRecompiledLoader', '--parallel', "$Jobs") $workspace
Set-Status "DONE: $build\$module.dll"

# 4. Play\: the loader exe, the engine module and the controller profile
$play = Join-Path $root $PlayDir
[IO.Directory]::CreateDirectory($play) | Out-Null
$playExe = [IO.Path]::GetFullPath((Join-Path $play $playExeName))
Get-Process ([IO.Path]::GetFileNameWithoutExtension($playExeName)) -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and [IO.Path]::GetFullPath($_.Path) -ieq $playExe } | Stop-Process -Force
Copy-Item -LiteralPath (Join-Path $build "$module.dll") -Destination (Join-Path $play "$module.dll") -Force
Copy-Item -LiteralPath (Join-Path $build $playExeName) -Destination $playExe -Force
# The engine's own files beside it: Dawn / SDL / dxcompiler DLLs, the DSP coefficient ROM, the pipeline cache seed
# and the Wii system files the NAND HLE starts from.
Get-ChildItem -LiteralPath $build -Filter '*.dll' | Where-Object { $_.Name -ne "$module.dll" } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $play $_.Name) -Force }
foreach ($f in 'dsp_coef.bin', 'initial_pipeline_cache.db') {
    if (Test-Path -LiteralPath (Join-Path $build $f)) { Copy-Item -LiteralPath (Join-Path $build $f) -Destination (Join-Path $play $f) -Force }
}
foreach ($d in 'wii_bootstrap', 'startup') {
    if (Test-Path -LiteralPath (Join-Path $build $d)) {
        if (Test-Path -LiteralPath (Join-Path $play $d)) { Remove-Item -LiteralPath (Join-Path $play $d) -Recurse -Force }
        Copy-Item -LiteralPath (Join-Path $build $d) -Destination (Join-Path $play $d) -Recurse -Force
    }
}
# nodtool (github.com/encounter/nod, MIT / Apache-2.0): the start screen sets the game up from .rvz / .wia / .gcz
# images with it (game_folder.cpp). A pinned version, fetched once into toolchain\.
$nodVersion = 'v2.0.0-alpha.10'
$nodSha256 = '1e150e33b88157527f96caea89ad12267aed8b117180c9ebaa89ebe89c1d948d'
$nodExe = Join-Path $toolchain "nodtool-$nodVersion.exe"
if (-not (Test-Path -LiteralPath $nodExe)) {
    Set-Status "nodtool: downloading $nodVersion"
    & curl.exe -fsSL -o "$nodExe.tmp" "https://github.com/encounter/nod/releases/download/$nodVersion/nodtool-windows-x86_64.exe"
    if ($LASTEXITCODE -eq 0 -and (Get-FileHash -Algorithm SHA256 -LiteralPath "$nodExe.tmp").Hash -ieq $nodSha256) {
        Move-Item -LiteralPath "$nodExe.tmp" -Destination $nodExe -Force
    } else {
        Remove-Item -LiteralPath "$nodExe.tmp" -ErrorAction SilentlyContinue
        Set-Status 'nodtool: download failed or did not match its pinned hash; .rvz images need converting to .iso'
    }
}
if (Test-Path -LiteralPath $nodExe) { Copy-Item -LiteralPath $nodExe -Destination (Join-Path $play 'nodtool.exe') -Force }
if (-not (Test-Path -LiteralPath (Join-Path $play 'Metroid.ini'))) {   # never overwrite the player's own edits
    Copy-Item -LiteralPath (Join-Path $root 'project\controls\Metroid.ini') -Destination (Join-Path $play 'Metroid.ini')
}
Set-Status "copied to $PlayDir\$module.dll and $PlayDir\$playExeName"
# The on-screen button prompts are drawn by the game itself from the disc (runtime/src/button_icons.cpp).

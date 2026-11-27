# Packs the Windows build (tools\build-otherm.ps1) into the release zip:
#   powershell -ExecutionPolicy Bypass -File tools\package-windows.ps1 -Version v1.0.0
# Output: dist\OtherMRecompiled-<version>-windows-x64.zip (the name the in-game updater fetches; one folder,
# OtherMRecompiled\, with the program and its files side by side) and dist\SHA256SUMS-windows.txt.
# The zip holds the program (the game's code in recompiled form, for the player whose disc it was built from) and the
# port's own files, never the disc's: the start screen sets the game up from the player's disc image into Disc\DATA
# beside the program. UserData\ (saves, settings, logs) and Metroid.ini are the player's and are not in the zip, so an
# update copied over the folder keeps them. Downloads (once, into toolchain\downloads): Dawn's and nod's license texts.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^v\d+\.\d+\.\d+[a-z]?$') { throw '-Version vX.Y.Z is required' }
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$downloads = Join-Path $root 'toolchain\downloads'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'windows'
$app = Join-Path $stage 'OtherMRecompiled'
$zip = Join-Path $dist "OtherMRecompiled-$Version-windows-x64.zip"
foreach ($f in 'MetroidOtherM.exe', 'engine_usa.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $build $f))) { throw "no build\${f}: run tools\build-otherm.ps1 first" }
}
$nodVersion = 'v2.0.0-alpha.10'   # the same pin as tools\build-otherm.ps1
$nodExe = Join-Path $root "toolchain\nodtool-$nodVersion.exe"
if (-not (Test-Path -LiteralPath $nodExe)) { throw "no ${nodExe}: tools\build-otherm.ps1 fetches it" }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $app 'licenses') | Out-Null
[IO.Directory]::CreateDirectory($downloads) | Out-Null

# The program, the engine module and every DLL the build put beside them (Dawn, SDL3, dxcompiler, the C++ runtime),
# the DSP coefficient ROM, the pipeline cache seed, the Wii system files the NAND HLE starts from, the menu emblem.
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe', '.dll' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $app }
foreach ($f in 'dsp_coef.bin', 'initial_pipeline_cache.db') {
    if (Test-Path -LiteralPath (Join-Path $build $f)) { Copy-Item -LiteralPath (Join-Path $build $f) -Destination $app }
}
foreach ($d in 'wii_bootstrap', 'startup') { Copy-Item -LiteralPath (Join-Path $build $d) -Destination $app -Recurse }
Copy-Item -LiteralPath $nodExe -Destination (Join-Path $app 'nodtool.exe')
Copy-Item -LiteralPath (Join-Path $root 'recomplauncher.json') -Destination $app   # RecompLauncher reads it beside the program
Set-Content -LiteralPath (Join-Path $app 'portable.txt') -Encoding ascii `
    -Value 'Portable mode: settings, saves and logs go to UserData\ next to the game.'
Set-Content -LiteralPath (Join-Path $app 'version.txt') -Encoding ascii -Value $Version   # the updater compares it
$commit = (& git -C $root rev-parse --short HEAD).Trim()
$dirty = if ((& git -C $root -c core.autocrlf=true -c core.filemode=false status --porcelain --untracked-files=no)) { '+changes' } else { '' }
$built = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
Set-Content -LiteralPath (Join-Path $app 'build-fingerprint.json') -Encoding ascii -Value @(
    '{', "  `"SetupVersion`": `"Other M Recompiled $Version ($commit$dirty, built $built, Windows x64 zip)`"", '}')

# Licenses: the port (GPLv3), the components in the program, the dependency trees, Dawn, nodtool.
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $app 'LICENSE.txt')
$lic = Join-Path $app 'licenses'
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination $lic
Get-ChildItem -LiteralPath (Join-Path $root 'dependencies') -Directory |
    Where-Object { $_.Name -notin 'dawn_prebuilt', 'cppwinrt' } | ForEach-Object {
        $name = $_.Name
        Get-ChildItem -LiteralPath $_.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' } |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $lic "$name-$($_.BaseName).txt") }
    }
foreach ($l in @(@('dawn-LICENSE', 'https://raw.githubusercontent.com/google/dawn/main/LICENSE'),
                 @('nod-LICENSE-MIT', "https://raw.githubusercontent.com/encounter/nod/$nodVersion/LICENSE-MIT"),
                 @('nod-LICENSE-APACHE', "https://raw.githubusercontent.com/encounter/nod/$nodVersion/LICENSE-APACHE"))) {
    $file = Join-Path $downloads $l[0]
    if (-not (Test-Path -LiteralPath $file)) {
        & curl.exe -fsSL -o $file $l[1]
        if ($LASTEXITCODE -ne 0) { throw "could not download $($l[1])" }
    }
}
Copy-Item -LiteralPath (Join-Path $downloads 'dawn-LICENSE') -Destination (Join-Path $lic 'dawn-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $downloads 'nod-LICENSE-MIT') -Destination (Join-Path $lic 'nodtool-LICENSE-MIT.txt')
Copy-Item -LiteralPath (Join-Path $downloads 'nod-LICENSE-APACHE') -Destination (Join-Path $lic 'nodtool-LICENSE-APACHE.txt')

Set-Content -LiteralPath (Join-Path $app 'README.txt') -Encoding ascii -Value @(
    "Other M Recompiled $Version for Windows (x64)",
    '',
    'Start MetroidOtherM.exe. The start screen asks for the folder that holds your own disc image of Metroid: Other M',
    '(USA, R3OE01, revision 1: .rvz, .iso, .wia, .gcz, .ciso or .wbfs) and sets the game up from it once, into Disc\DATA',
    'here. Saves, settings and logs go to UserData\ here; Metroid.ini holds your controls once you change them.',
    '',
    'Updates: the start screen offers new versions and installs them over this folder, keeping UserData, Disc and',
    'Metroid.ini. Mods (Other M Redux and others) go in UserData\mods.',
    '',
    'Problems: https://github.com/gminustwo/otherm-recomp/issues (the Bug report form; attach the log from UserData).',
    'No game data is included: this folder holds the game''s code in recompiled form and the port''s own files only.')

# Guard: no disc data in the download.
$bad = Get-ChildItem -LiteralPath $app -Recurse | Where-Object {
    ($_.PSIsContainer -and $_.Name -in 'Disc', 'DATA', 'sys', 'files', 'UserData') -or
    (-not $_.PSIsContainer -and $_.Extension -in '.rvz', '.iso', '.wbfs', '.dol', '.rel', '.state') -or $_.Name -eq 'Metroid.ini' }
if ($bad) { throw "disc or player data in the zip: $($bad.FullName -join ', ')" }

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Push-Location $stage
# Windows' own bsdtar writes zips (-a picks the format from the name); a GNU tar earlier on PATH (Git's) can't.
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
try { & $tar -a -c -f $zip 'OtherMRecompiled'; if ($LASTEXITCODE -ne 0) { throw 'tar failed' } } finally { Pop-Location }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $dist 'SHA256SUMS-windows.txt') -Encoding ascii -Value "$hash  $(Split-Path -Leaf $zip)"
Write-Host "wrote $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB)"

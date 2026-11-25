# Prepares everything the translation reads from your own disc of Metroid: Other M (USA, R3OE01, revision 1):
#   Disc\DATA                     the disc's DATA partition (DolphinTool, from .rvz / .iso / .wbfs / .ciso)
#   game\main.dol                 checked against the hash recomp.yml pins
#   game\otherm_linked.rel        files\rel\Release\game.rel linked offline at the pinned address, plus its layout json
#   project\MAP.txt       function starts (gen_function_map.py), named from the project's names files
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File <project>\tools\setup-otherm.ps1 [-Image <disc>] [-DolphinDir <dir>]
[CmdletBinding()]
param(
    [string]$Image = '',        # default: the one .rvz/.iso/.wbfs/.ciso in the project folder
    [string]$DolphinDir = '',   # folder with DolphinTool.exe; default: $env:DOLPHIN_DIR, PATH
    [switch]$SkipExtract
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root 'project'
$disc = Join-Path $root 'Disc\DATA'
$game = Join-Path $root 'game'
$expectedDolHash = '8aa8be89f491ddc70ba52efb42171140184c0ad25fe056b0e08e8eaa67841a22'
$python = (Get-Command python -ErrorAction Stop).Source

function Invoke-Python([string[]]$Arguments) {
    & $python -I @Arguments
    if ($LASTEXITCODE -ne 0) { throw "python $($Arguments[0]) failed (exit $LASTEXITCODE)" }
}

if (-not $DolphinDir) { $DolphinDir = $env:DOLPHIN_DIR }
if (-not $DolphinDir) {
    $tool = Get-Command DolphinTool.exe -ErrorAction SilentlyContinue
    if ($tool) { $DolphinDir = Split-Path -Parent $tool.Source }
}

# 1. Disc
if (-not $SkipExtract -and -not (Test-Path -LiteralPath (Join-Path $disc 'sys\main.dol'))) {
    if (-not $Image) {
        $images = @(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Extension -in '.rvz', '.iso', '.wbfs', '.ciso', '.gcz', '.wia' })
        if ($images.Count -ne 1) { throw "Pass -Image <your disc image> (found $($images.Count) candidates in $root)." }
        $Image = $images[0].FullName
    }
    if (-not $DolphinDir -or -not (Test-Path -LiteralPath (Join-Path $DolphinDir 'DolphinTool.exe'))) {
        throw 'DolphinTool.exe not found: pass -DolphinDir <Dolphin folder> or set DOLPHIN_DIR.'
    }
    Write-Host "extracting $Image (DATA partition) to $disc"
    # DolphinTool writes the partition as <out>\DATA.
    & (Join-Path $DolphinDir 'DolphinTool.exe') extract -i $Image -g -q -o (Join-Path $root 'Disc')
    if ($LASTEXITCODE -ne 0) { throw "DolphinTool extract failed (exit $LASTEXITCODE)" }
}
# The disc header carries the revision; the partition's own sys\boot.bin says 0 on this disc. The main.dol hash
# below is what really pins the version.
$header = Join-Path $disc 'disc\header.bin'
if (-not (Test-Path -LiteralPath $header)) { $header = Join-Path $disc 'sys\boot.bin' }
$boot = [IO.File]::ReadAllBytes($header)
$gameId = [Text.Encoding]::ASCII.GetString($boot, 0, 6)
if ($gameId -ne 'R3OE01') { throw "Disc\DATA is $gameId; this port needs Metroid: Other M, USA (R3OE01) revision 1." }
Write-Host "disc: $gameId revision $($boot[7])"

# 2. main.dol
[IO.Directory]::CreateDirectory($game) | Out-Null
Copy-Item -LiteralPath (Join-Path $disc 'sys\main.dol') -Destination (Join-Path $game 'main.dol') -Force
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $game 'main.dol')).Hash.ToLowerInvariant()
if ($hash -ne $expectedDolHash) { throw "main.dol hash $hash is not R3OE01 rev 1's ($expectedDolHash)." }

# 3. game.rel, linked for the pinned address (prelink_otherm_rel.py DEFAULT_IMAGE)
Invoke-Python @((Join-Path $proj 'tools\prelink_otherm_rel.py'), (Join-Path $disc 'files\rel\Release\game.rel'),
    (Join-Path $game 'otherm_linked.rel'), (Join-Path $game 'otherm_rel_layout.json'))

# 4. Function map, named from the project's names files
$mapArgs = @((Join-Path $proj 'tools\gen_function_map.py'), (Join-Path $game 'main.dol'), (Join-Path $game 'otherm_linked.rel'),
    (Join-Path $game 'otherm_rel_layout.json'), (Join-Path $proj 'MAP.txt'), (Join-Path $proj 'names_align.txt'), (Join-Path $proj 'names_sig.txt'),
    (Join-Path $proj 'names_pattern.txt'), (Join-Path $proj 'names.txt'))
Invoke-Python $mapArgs
Write-Host 'setup done: next, tools\build-otherm.ps1'

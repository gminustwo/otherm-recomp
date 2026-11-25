# Starts Play\MetroidOtherM.exe for a bring-up run: stdout/stderr to build-logs\run_<tag>.log, PNG shots of the game
# window (PrintWindow: other windows on top do not matter) at the given seconds into build-logs\shots\<tag>_<sec>.png,
# then stops it. Usage: run-otherm.ps1 [-Tag name] [-Seconds 60] [-ShotAt 10,30,60] [-Env @('OTHERM_X=1')] [-PlayDir Play]
# Only a game running from that folder is closed first: a copy the player has open elsewhere is left alone.
# The test window stays in the background: it opens without taking focus, is put behind every other window, and
# ignores controllers while unfocused, so whatever the player is doing meanwhile never reaches the test game.
[CmdletBinding()]
param([string]$Tag = 'run', [int]$Seconds = 60, [string]$ShotAt = '10,30,60', [string[]]$Env = @(), [string]$PlayDir = 'Play')
# -ShotAt is a string ("8,20,40") because powershell -File hands every argument over as text.
$shotTimes = @($ShotAt -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })  # a typed parameter keeps its type
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$play = Join-Path $root $PlayDir
$logs = Join-Path $root 'build-logs'
$shots = Join-Path $logs 'shots'
[IO.Directory]::CreateDirectory($shots) | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
[W]::SetProcessDPIAware() | Out-Null
# Test runs boot straight into the game, past the start screen; -Env 'OTHERM_SKIP_LAUNCHER=0' shows it.
[Environment]::SetEnvironmentVariable('OTHERM_SKIP_LAUNCHER', '1', 'Process')
# -Env "A=1;B=2": one string, ';'-separated, because powershell -File joins what bash passes into one argument.
foreach ($e in @($Env -join ';' -split ';' | Where-Object { $_ })) { $k, $v = $e -split '=', 2; [Environment]::SetEnvironmentVariable($k, $v, 'Process') }
$exe = [IO.Path]::GetFullPath((Join-Path $play 'MetroidOtherM.exe'))
# SDL hints from the environment outrank the engine's own (aurora asks for background controller input).
[Environment]::SetEnvironmentVariable('SDL_WINDOW_ACTIVATE_WHEN_SHOWN', '0', 'Process')
[Environment]::SetEnvironmentVariable('SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS', '0', 'Process')
Get-Process MetroidOtherM -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and [IO.Path]::GetFullPath($_.Path) -ieq $exe } | Stop-Process -Force
$out = Join-Path $logs "run_$Tag.log"
$err = Join-Path $logs "run_$Tag.err.log"
$p = Start-Process -FilePath $exe -WorkingDirectory $play -PassThru `
    -RedirectStandardOutput $out -RedirectStandardError $err
$start = Get-Date
# Behind every other window, without activating it (HWND_BOTTOM; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE). PrintWindow
# still captures it there.
$sunk = $false
function Sink-Window {
    if ($script:sunk -or $p.HasExited) { return }
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { [W]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0, 0, 0, 0, 0x13) | Out-Null; $script:sunk = $true }
}
foreach ($t in ($shotTimes | Sort-Object)) {
    while (((Get-Date) - $start).TotalSeconds -lt $t -and -not $p.HasExited) { Sink-Window; Start-Sleep -Milliseconds 200 }
    if ($p.HasExited) { break }
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq 0) { Write-Host "${t}s: no window yet"; continue }
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { Write-Host "${t}s: empty window"; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [W]::PrintWindow($h, $dc, 3) | Out-Null
    $g.ReleaseHdc($dc)
    $file = Join-Path $shots ("{0}_{1:D3}.png" -f $Tag, $t)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "${t}s: $file (${w}x${hh})"
}
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) { Sink-Window; Start-Sleep -Milliseconds 200 }
if (-not $p.HasExited) {
    # A guest thread spinning in translated code keeps the process alive through Kill's grace: force it.
    & taskkill /PID $p.Id /F /T 2>$null | Out-Null
    Write-Host "stopped after $Seconds s"
} else { Write-Host "exited with code $($p.ExitCode)" }
Write-Host "logs: $out / $err"

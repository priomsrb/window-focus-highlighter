# Regenerates docs/img/{appearance,timing,behavior}{,-light}.png from the built exe.
# Temporarily replaces settings.ini with defaults and flips the Windows app theme; both are restored afterwards.
# If the sidebar layout changes, adjust the three $pages Y offsets (client pixels from the window's top).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(int f, int dx, int dy, int d, int e);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDPIAware() | Out-Null

$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'WindowFocusHighlighter.exe'
$outDir = Join-Path $root 'docs\img'
$ini = Join-Path $env:APPDATA 'WindowFocusHighlighter\settings.ini'
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$pages = [ordered]@{ appearance = 193; timing = 263; behavior = 333 }
$defaults = "[Highlight]`r`nEnabled=1`r`nMode=0`r`nOpacity=90`r`nTintOpacity=20`r`nThickness=4`r`nSoftness=50`r`nHoldMs=350`r`nFadeMs=250`r`nHotkey=1608`r`nColor=3399FF`r`nIgnore=`r`n"

$origTheme = (Get-ItemProperty $key).AppsUseLightTheme
$origIni = if (Test-Path $ini) { [IO.File]::ReadAllBytes($ini) } else { $null }

function Stop-App { Stop-Process -Name WindowFocusHighlighter -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }

try {
    foreach ($theme in 'dark', 'light') {
        Stop-App
        Set-ItemProperty $key AppsUseLightTheme $(if ($theme -eq 'light') { 1 } else { 0 })
        [IO.File]::WriteAllText($ini, $defaults)
        Start-Process $exe; Start-Sleep -Milliseconds 800
        Start-Process $exe; Start-Sleep -Milliseconds 1500   # second launch opens settings in the first
        $h = (Get-Process WindowFocusHighlighter | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
        if (-not $h) { throw 'settings window did not open' }
        [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 500
        $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $ht = $r.B - $r.T
        foreach ($p in $pages.Keys) {
            $null = [W]::SetCursorPos($r.L + 100, $r.T + $pages[$p]); Start-Sleep -Milliseconds 200
            [W]::mouse_event(2, 0, 0, 0, 0); [W]::mouse_event(4, 0, 0, 0, 0); Start-Sleep -Milliseconds 300
            $null = [W]::SetCursorPos($r.L + $w - 30, $r.T + $ht - 30); Start-Sleep -Milliseconds 600   # park cursor, no hover
            $bmp = New-Object Drawing.Bitmap $w, $ht
            $g = [Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
            $suffix = if ($theme -eq 'light') { '-light' } else { '' }
            $bmp.Save((Join-Path $outDir "$p$suffix.png"), [Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
        }
    }
}
finally {
    Stop-App
    Set-ItemProperty $key AppsUseLightTheme $origTheme
    if ($null -ne $origIni) { [IO.File]::WriteAllBytes($ini, $origIni) }
    Start-Process $exe; Start-Sleep -Milliseconds 800; Start-Process $exe   # leave app running with settings open
}
Write-Host "Screenshots written to $outDir"

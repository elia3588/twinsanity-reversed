param(
    [string]$Hold = "",      # key name to hold, e.g. W
    [int]$HoldMs = 0,
    [int]$Dx = 0,            # mouse movement in total
    [int]$Dy = 0,
    [int]$Steps = 10
)
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Inp {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

# The game's own window (an SDL window titled "Crash Twinsanity"); the process's MainWindowHandle is the console's, so the
# title is looked for instead
function Find-GameWindow {
    $found = [IntPtr]::Zero
    $cb = [Inp+EnumProc]{
        param($h, $p)
        if (-not [Inp]::IsWindowVisible($h)) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [Inp]::GetWindowText($h, $sb, 256) | Out-Null
        if ($sb.ToString() -like "*Crash Twinsanity*") { $script:found = $h; return $false }
        return $true
    }
    [Inp]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

$h = Find-GameWindow
if ($h -eq [IntPtr]::Zero) {
    $p = Get-Process twinsanity -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p -ne $null) { $h = $p.MainWindowHandle }
}
if ($h -eq [IntPtr]::Zero) { Write-Output "no-window"; exit 1 }
[Inp]::ShowWindow($h, 9) | Out-Null
[Inp]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

if ($Hold -ne "") {
    $vk = 0
    switch ($Hold.ToUpper()) {
        "W" { $vk = 0x57 } "A" { $vk = 0x41 } "S" { $vk = 0x53 } "D" { $vk = 0x44 }
        "UP" { $vk = 0x26 } "DOWN" { $vk = 0x28 } "LEFT" { $vk = 0x25 } "RIGHT" { $vk = 0x27 }
        "SPACE" { $vk = 0x20 } "ENTER" { $vk = 0x0D }
    }
    if ($vk -ne 0) {
        [Inp]::keybd_event($vk, 0, 0, [IntPtr]::Zero)
        Start-Sleep -Milliseconds $HoldMs
        [Inp]::keybd_event($vk, 0, 2, [IntPtr]::Zero)
    }
}

if ($Dx -ne 0 -or $Dy -ne 0) {
    $r = New-Object Inp+RECT
    [Inp]::GetClientRect($h, [ref]$r) | Out-Null
    $origin = New-Object Inp+POINT
    [Inp]::ClientToScreen($h, [ref]$origin) | Out-Null
    $cx = $origin.X + [int](($r.Right - $r.Left) / 2)
    $cy = $origin.Y + [int](($r.Bottom - $r.Top) / 2)
    [Inp]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 120
    $stepX = $Dx / $Steps
    $stepY = $Dy / $Steps
    for ($i = 0; $i -lt $Steps; $i++) {
        $cx += $stepX
        $cy += $stepY
        [Inp]::SetCursorPos([int]$cx, [int]$cy) | Out-Null
        Start-Sleep -Milliseconds 25
    }
}
Write-Output "done"

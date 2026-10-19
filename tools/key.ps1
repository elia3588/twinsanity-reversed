param(
    [string]$Keys = "ENTER",   # e.g. "ENTER", "DOWN", or "DOWN,ENTER"
    [int]$HoldMs = 120
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Key {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$h = [IntPtr]::Zero
$proc = Get-Process twinsanity -ErrorAction SilentlyContinue | Select-Object -First 1
if ($proc -ne $null) { $h = $proc.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { $h = [Key]::FindWindow($null, "Crash Twinsanity") }
if ($h -eq [IntPtr]::Zero) { Write-Output "no-window"; exit 1 }

# Take the foreground properly: a background process can't steal it on its own, so this thread is attached to the current
# foreground thread and the target's for the moment of the switch
$fg = [Key]::GetForegroundWindow()
$cur = [Key]::GetCurrentThreadId()
$fgThread = if ($fg -ne [IntPtr]::Zero) { [Key]::GetWindowThreadProcessId($fg, [ref]([uint32]0)) } else { 0 }
$targetPid = [uint32]0
$targetThread = [Key]::GetWindowThreadProcessId($h, [ref]$targetPid)
if ($fgThread -ne 0) { [Key]::AttachThreadInput($cur, $fgThread, $true) | Out-Null }
if ($targetThread -ne 0) { [Key]::AttachThreadInput($cur, $targetThread, $true) | Out-Null }
[Key]::ShowWindow($h, 9) | Out-Null
[Key]::BringWindowToTop($h) | Out-Null
[Key]::SetForegroundWindow($h) | Out-Null
[Key]::SetActiveWindow($h) | Out-Null
[Key]::SetFocus($h) | Out-Null
try { [Key]::SetForegroundWindow($h) | Out-Null } catch {}
if ($targetThread -ne 0) { [Key]::AttachThreadInput($cur, $targetThread, $false) | Out-Null }
if ($fgThread -ne 0) { [Key]::AttachThreadInput($cur, $fgThread, $false) | Out-Null }
Start-Sleep -Milliseconds 200

# A real click on the window's middle, which also makes it the active one when the foreground couldn't be taken outright
$r = New-Object Key+RECT
if ([Key]::GetWindowRect($h, [ref]$r)) {
    [Key]::SetCursorPos([int](($r.Left + $r.Right) / 2), [int](($r.Top + $r.Bottom) / 2)) | Out-Null
    Start-Sleep -Milliseconds 120
    [Key]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    [Key]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}

$fgAfter = [Key]::GetForegroundWindow()
Write-Output ("focus " + ($fgAfter -eq $h))
Start-Sleep -Milliseconds 150

$map = @{
    "ENTER" = 0x0D; "RETURN" = 0x0D; "SPACE" = 0x20; "ESC" = 0x1B; "BACK" = 0x08
    "UP" = 0x26; "DOWN" = 0x28; "LEFT" = 0x25; "RIGHT" = 0x27
    "W" = 0x57; "A" = 0x41; "S" = 0x53; "D" = 0x44
    "Z" = 0x5A; "X" = 0x58; "C" = 0x43; "V" = 0x56; "Q" = 0x51; "E" = 0x45
}
foreach ($k in $Keys.Split(",")) {
    $name = $k.Trim().ToUpper()
    if (-not $map.ContainsKey($name)) { Write-Output "unknown:$name"; continue }
    $vk = $map[$name]
    [Key]::keybd_event([byte]$vk, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [Key]::keybd_event([byte]$vk, 0, 2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
}
Write-Output "sent"

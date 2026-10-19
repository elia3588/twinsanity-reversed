param([int]$Wait = 44, [string]$Out = "C:\msys64\tmp\opencode\shot.png")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
$env:TWINSANITY_DISC = 'C:\Users\Elia\Desktop\twins\twinsanity-reversed\disc'
$p = Start-Process -FilePath '.\build\desktop\twinsanity.exe' -WorkingDirectory '.\build\desktop' -PassThru
Start-Sleep -Seconds $Wait
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -ne [IntPtr]::Zero) {
    [Win]::ShowWindow($h, 9) | Out-Null
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 800
}
powershell -ExecutionPolicy Bypass -File .\tools\screenshot.ps1 -Out $Out

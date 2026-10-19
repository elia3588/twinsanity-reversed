param([int]$Wait = 30)
Start-Process taskkill -ArgumentList '/F','/IM','twinsanity.exe' -WindowStyle Hidden
Start-Sleep -Seconds 1
$env:TWINSANITY_DISC = 'C:\Users\Elia\Desktop\twins\twinsanity-reversed\disc'
$p = Start-Process -FilePath 'C:\Users\Elia\Desktop\twins\twinsanity-reversed\build\desktop\twinsanity.exe' -WorkingDirectory 'C:\Users\Elia\Desktop\twins\twinsanity-reversed\build\desktop' -PassThru -RedirectStandardOutput 'C:\msys64\tmp\opencode\run.out' -RedirectStandardError 'C:\msys64\tmp\opencode\run.err'
Start-Sleep -Seconds $Wait
if ($p.HasExited) { Write-Output 'exited' }
else { Stop-Process -Id $p.Id -Force; Write-Output 'killed' }

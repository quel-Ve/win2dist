@echo off
:: remove Start Menu shortcut and logon task
:: self-elevate (task deletion needs admin)
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
schtasks /delete /tn "win2blur" /f >nul 2>&1
powershell -ExecutionPolicy Bypass -Command "$f=[Environment]::GetFolderPath('Programs')+'\win2blur.lnk';if(Test-Path $f){Remove-Item $f -Force;Write-Host 'Removed.'}else{Write-Host 'Not found.'}"
pause

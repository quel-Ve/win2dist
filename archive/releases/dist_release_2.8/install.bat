@echo off
:: win2blur setup - one-click admin shortcut + silent logon auto-start
:: 1) Register an elevated logon task (win2blur auto-starts silently at logon)
:: 2) Start Menu shortcut -> launches that task (one click = admin, no UAC)

:: self-elevate (creating a highest-privilege task requires admin)
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "TARGET=%~dp0win2blur.exe"
if not exist "%TARGET%" (
    echo ERROR: win2blur.exe not found next to this script.
    pause
    exit /b 1
)

:: 1) elevated logon task - win2blur starts at logon with no UAC prompt
schtasks /create /tn "win2blur" /tr "\"%TARGET%\"" /sc onlogon /ru "%USERNAME%" /rl highest /f
if %errorlevel% neq 0 (
    echo WARNING: failed to register logon task.
)

:: 2) Start Menu shortcut -> silent elevated launch (via the task)
powershell -ExecutionPolicy Bypass -Command "$d=[Environment]::GetFolderPath('Programs');$W=(New-Object -ComObject WScript.Shell).CreateShortcut(\"$d\win2blur.lnk\");$W.TargetPath='%~dp0launch_win2blur.vbs';$W.WorkingDirectory='%~dp0';$W.IconLocation='%TARGET%,0';$W.Description='win2blur - Window transparency + Acrylic frosted glass (admin)';$W.Save();Write-Host 'Start Menu shortcut added - one click launches win2blur elevated.'"
pause

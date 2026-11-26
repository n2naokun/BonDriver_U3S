@echo off
setlocal DisableDelayedExpansion
powershell.exe -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true); if ($key) { $key.DeleteValue('U3SHost', $false) }"
if errorlevel 1 (
    echo Startup registration could not be removed.
    exit /b 1
)
echo Startup registration removed.
exit /b 0

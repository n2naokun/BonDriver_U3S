@echo off
setlocal DisableDelayedExpansion
set "U3S_HOST_EXE=%~dp0U3SHost.exe"
if not exist "%U3S_HOST_EXE%" (
    echo U3SHost.exe was not found. Startup registration is unchanged.
    exit /b 1
)
powershell.exe -NoProfile -NonInteractive -Command "$ErrorActionPreference = 'Stop'; $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run'); $q = [char]34; $key.SetValue('U3SHost', ([string]$q + $env:U3S_HOST_EXE + $q), [Microsoft.Win32.RegistryValueKind]::String)"
if errorlevel 1 (
    echo Startup registration failed.
    exit /b 1
)
echo Startup registration succeeded.
start "" /wait "%U3S_HOST_EXE%" --start
if errorlevel 1 (
    echo U3SHost did not accept connections. Startup registration remains active.
    exit /b 1
)
echo U3SHost is accepting connections.
exit /b 0

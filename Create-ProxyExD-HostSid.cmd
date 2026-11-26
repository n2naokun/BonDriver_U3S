@echo off
setlocal DisableDelayedExpansion
set "U3S_HOST_SID_REG=%~dp0Set-ProxyExD-HostSid.reg"
set "U3S_HOST_SID_REMOVE_REG=%~dp0Remove-ProxyExD-HostSid.reg"
powershell.exe -NoProfile -NonInteractive -Command ^
  "$ErrorActionPreference = 'Stop'; $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value; $name = [string][char]34 + 'HostSid' + [char]34; $q = [char]34; $nl = [Environment]::NewLine;" ^
  "$header = ('Windows Registry Editor Version 5.00', '', '[HKEY_LOCAL_MACHINE\SOFTWARE\U3SHost]' -join $nl) + $nl;" ^
  "$files = [ordered]@{ ($env:U3S_HOST_SID_REG) = $header + $name + '=' + $q + $sid + $q + $nl; ($env:U3S_HOST_SID_REMOVE_REG) = $header + $name + '=-' + $nl };" ^
  "foreach ($path in $files.Keys) { if ([IO.File]::Exists($path) -and [IO.File]::ReadAllText($path) -cne $files[$path]) { throw ('Move the existing file before generating a new one: ' + $path) } };" ^
  "foreach ($path in $files.Keys) { if (![IO.File]::Exists($path)) { [IO.File]::WriteAllText($path, $files[$path], [Text.Encoding]::Unicode) }; Write-Output $path }"
if errorlevel 1 exit /b 1
exit /b 0

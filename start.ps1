$ErrorActionPreference = 'Stop'
$program = Join-Path $PSScriptRoot 'dist\CodexLight.exe'
if (-not (Test-Path -LiteralPath $program)) { throw 'Please build native\build.cmd first.' }
Start-Process -FilePath $program -WorkingDirectory $PSScriptRoot -WindowStyle Hidden

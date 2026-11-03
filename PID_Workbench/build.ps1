$ErrorActionPreference = 'Stop'
$sourceProtocol = Join-Path $PSScriptRoot '../PID_Shared/PIDFlash/tools'
$bundledProtocol = Join-Path $PSScriptRoot 'backend/protocol'
New-Item -ItemType Directory -Path $bundledProtocol -Force | Out-Null
foreach ($protocolFile in @('flash_stm32.py','echo_stm32.py','bulk_stm32.py')) {
    Copy-Item -LiteralPath (Join-Path $sourceProtocol $protocolFile) -Destination (Join-Path $bundledProtocol $protocolFile) -Force
}
$compilerPath = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path -LiteralPath $compilerPath)) { $compilerPath = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe' }
$sources = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cs' | ForEach-Object { $_.FullName }
& $compilerPath /nologo /target:winexe /optimize+ /warn:4 /codepage:65001 "/win32manifest:$PSScriptRoot/app.manifest" "/win32icon:$PSScriptRoot/assets/workbench.ico" "/out:$PSScriptRoot/PID_Workbench.exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll $sources
if ($LASTEXITCODE -ne 0) { throw 'PID Workbench build failed' }
Write-Output 'Built PID_Workbench.exe'

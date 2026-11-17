param([string]$OutputPath = (Join-Path $PSScriptRoot 'PidKeilDownload.exe'))
$ErrorActionPreference = 'Stop'
$keilCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $keilCompiler)) {
    $keilCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
}
if (-not (Test-Path -LiteralPath $keilCompiler)) { throw '.NET Framework C# compiler not found' }
& $keilCompiler /nologo /target:winexe /optimize+ /platform:anycpu /codepage:65001 "/win32manifest:$PSScriptRoot\launcher.manifest" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/out:$OutputPath" "$PSScriptRoot\KeilDownloadLauncher.cs" "$PSScriptRoot\DownloadProgress.cs" "$PSScriptRoot\..\..\DpiSupport.cs"
if ($LASTEXITCODE -ne 0) { throw 'Keil launcher build failed' }
Write-Output "Built $OutputPath (Windows subsystem, no terminal window)"

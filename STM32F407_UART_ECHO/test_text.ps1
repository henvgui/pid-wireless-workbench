param(
    [string]$Text,
    [string]$File,
    [int]$Bytes = 131091,
    [switch]$Stress,
    [string]$Port = 'COM14'
)
$ErrorActionPreference = 'Stop'
$echoTool = Join-Path $PSScriptRoot '../PID_Shared/PIDFlash/tools/echo_stm32.py'
$echoPython = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/python.exe'
$echoResults = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force -Path $echoResults | Out-Null
$echoStamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$echoArgs = @($echoTool, '--part', 'STM32F407VET6', '--port', $Port, '--journal', (Join-Path $echoResults ($echoStamp + '.json')))
if ($PSBoundParameters.ContainsKey('Text') -and $PSBoundParameters.ContainsKey('File')) { throw 'Text and File cannot be combined.' }
if ($PSBoundParameters.ContainsKey('Text')) { $echoArgs += @('--text', $Text) }
elseif ($PSBoundParameters.ContainsKey('File')) { $echoArgs += @('--file', (Resolve-Path -LiteralPath $File).Path) }
else { $echoArgs += @('--bytes', [string]$Bytes) }
if ($Stress) { $echoArgs += @('--drop-every', '5', '--corrupt-uart-every', '7') }
& $echoPython @echoArgs
exit $LASTEXITCODE

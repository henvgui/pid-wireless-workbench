param(
    [string]$Text = '',
    [string]$File = '',
    [int]$Bytes = 131072,
    [switch]$Stress
)
$ErrorActionPreference = 'Stop'
$echoPython = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/python.exe'
$echoTool = Join-Path $PSScriptRoot '../PID_Shared/PIDFlash/tools/echo_stm32.py'
$echoStamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$echoJournal = Join-Path $PSScriptRoot "results/echo_$echoStamp.json"
$echoArgs = @($echoTool, '--journal', $echoJournal)
if ($File) {
    $echoArgs += @('--file', (Resolve-Path -LiteralPath $File).Path)
} elseif ($Text) {
    $echoArgs += @('--text', $Text)
} else {
    $echoArgs += @('--bytes', "$Bytes")
}
if ($Stress) { $echoArgs += @('--drop-every', '5', '--corrupt-uart-every', '7') }
& $echoPython @echoArgs
exit $LASTEXITCODE

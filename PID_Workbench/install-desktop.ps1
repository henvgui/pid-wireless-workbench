$ErrorActionPreference = 'Stop'
$taskApp = Join-Path $PSScriptRoot 'PID_Workbench.exe'
$taskIcon = Join-Path $PSScriptRoot 'assets/workbench.ico'
if (-not (Test-Path -LiteralPath $taskApp)) { throw 'Build PID_Workbench.exe first with build.ps1.' }
if (-not (Test-Path -LiteralPath $taskIcon)) { throw 'Missing assets/workbench.ico.' }
$taskDesktop = [Environment]::GetFolderPath('Desktop')
$taskLinkPath = Join-Path $taskDesktop 'PID 调试工作台.lnk'
$taskShell = New-Object -ComObject WScript.Shell
$taskLink = $taskShell.CreateShortcut($taskLinkPath)
if ($taskLink.TargetPath -and $taskLink.TargetPath -ne $taskApp) { throw 'A shortcut with this name points to another application.' }
$taskLink.TargetPath = $taskApp
$taskLink.WorkingDirectory = $PSScriptRoot
$taskLink.Description = 'PID 调试工作台：无线烧录、PID 调参与无线串口'
$taskLink.IconLocation = "$taskIcon,0"
$taskLink.WindowStyle = 1
$taskLink.Save()
if (-not ('PidDesktopShell' -as [type])) {
    Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class PidDesktopShell { [DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern void SHChangeNotify(uint change, uint flags, string item, IntPtr other); }'
}
[PidDesktopShell]::SHChangeNotify(0x2000, 0x1005, $taskLinkPath, [IntPtr]::Zero)
Write-Output $taskLinkPath

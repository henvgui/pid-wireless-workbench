param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'output/release')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$output = [IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($OutputDirectory)).FullName
$package = 'PID-Workbench-Windows-x64.zip'
$staging = Join-Path ([IO.Path]::GetTempPath()) ('pid-workbench-package-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $staging | Out-Null

function Get-VerifiedArchive($Uri, $FileName, $Sha256) {
    $file = Join-Path $staging $FileName
    Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $file
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA256 mismatch for $FileName"
    }
    return $file
}

try {
    $pythonUrl = 'https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip'
    $pyserialUrl = 'https://files.pythonhosted.org/packages/07/bc/587a445451b253b285629263eb51c2d8e9bcea4fc97826266d186f96f558/pyserial-3.5-py2.py3-none-any.whl'
    $python = Get-VerifiedArchive $pythonUrl 'python.zip' '9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38'
    $pyserial = Get-VerifiedArchive $pyserialUrl 'pyserial.whl' 'c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0'

    $bundle = (New-Item -ItemType Directory -Path (Join-Path $staging 'PID-Workbench')).FullName
    $keil = [IO.Directory]::CreateDirectory((Join-Path $bundle 'integrations/keil')).FullName
    & (Join-Path $PSScriptRoot 'build.ps1') -OutputPath (Join-Path $bundle 'PID_Workbench.exe')
    & (Join-Path $PSScriptRoot 'integrations/keil/build_launcher.ps1') -OutputPath (Join-Path $keil 'PidKeilDownload.exe')
    $runtime = Join-Path $bundle 'runtime'
    [IO.Compression.ZipFile]::ExtractToDirectory($python, $runtime)
    [IO.Compression.ZipFile]::ExtractToDirectory($pyserial, (Join-Path $runtime 'packages'))
    $paths = @('python313.zip', '.', 'packages', '../backend', '../backend/protocol')
    Set-Content -LiteralPath (Join-Path $runtime 'python313._pth') -Value $paths -Encoding ascii

    $files = @(
        'PID_Workbench.exe.config', 'pidctl.cmd',
        'assets/workbench.ico', 'docs/WIRELESS_SERIAL.md',
        'backend/api_client.py', 'backend/pidctl.py', 'backend/server.py',
        'backend/serial_terminal.py', 'backend/workbench_core.py',
        'backend/protocol/flash_stm32.py', 'backend/protocol/echo_stm32.py', 'backend/protocol/bulk_stm32.py',
        'integrations/keil/PidKeilDownload.exe.config',
        'integrations/keil/configure_keil.py', 'integrations/keil/pid_keil.py'
    )
    foreach ($relativePath in $files) {
        $destination = Join-Path $bundle $relativePath
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $relativePath) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs/WINDOWS_QUICKSTART.md') -Destination (Join-Path $bundle 'START-HERE.md')
    # Keep the in-app help button usable in the portable layout.
    $aiGuide = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'AI_USAGE.md') -Raw -Encoding utf8
    $docsBase = 'https://github.com/henvgui/pid-wireless-workbench/blob/main/'
    $aiGuide = $aiGuide.Replace('](docs/', ('](' + $docsBase + 'PID_Workbench/docs/'))
    $aiGuide = $aiGuide.Replace('](../PID_Shared/', ('](' + $docsBase + 'PID_Shared/'))
    [IO.File]::WriteAllText((Join-Path $bundle 'AI_USAGE.md'), $aiGuide, [Text.UTF8Encoding]::new($false))
    $localPython = Join-Path $runtime 'python.exe'
    & $localPython -B -c 'import pyexpat, sqlite3, ssl, serial, server; assert serial.VERSION == "3.5"; print("Portable runtime OK")'
    if ($LASTEXITCODE -ne 0) { throw 'Portable runtime smoke test failed' }
    & $localPython -B (Join-Path $bundle 'backend/pidctl.py') --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Portable CLI smoke test failed' }
    & $localPython -B (Join-Path $bundle 'integrations/keil/pid_keil.py') --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Portable Keil adapter smoke test failed' }

    $zip = Join-Path $staging $package
    [IO.Compression.ZipFile]::CreateFromDirectory($bundle, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    $contents = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($entry in $contents.Entries) {
            if ($entry.FullName -match '(?i)(^|[/\\])(private|pair_keys\.h|api_token\.txt|__pycache__)([/\\]|$)') {
                throw 'Unexpected private or generated file in release'
            }
        }
    } finally {
        $contents.Dispose()
    }
    $checksum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $destinationZip = Join-Path $output $package
    Copy-Item -LiteralPath $zip -Destination $destinationZip -Force
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Value "$checksum  $package" -Encoding ascii
    Write-Output $destinationZip
} finally {
    $resolved = (Resolve-Path -LiteralPath $staging).Path
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    if ([IO.Path]::GetDirectoryName($resolved) -ieq $tempRoot -and
        [IO.Path]::GetFileName($resolved) -match '^pid-workbench-package-[0-9a-f-]{36}$') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

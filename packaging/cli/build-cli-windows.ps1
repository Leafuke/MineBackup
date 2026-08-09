param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version must use X.Y.Z: $Version"
}
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$binary = (Resolve-Path $Executable).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if ([System.IO.Path]::GetPathRoot($output) -eq $output) {
    throw 'The output directory cannot be a filesystem root.'
}
$stage = Join-Path $output ".cli-package-work-$PID"
$packageRoot = Join-Path $stage "MineBackup-CLI-$Version-windows-x64"
$archive = Join-Path $output "MineBackup-CLI-$Version-windows-x64.zip"

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $binary -Destination (Join-Path $packageRoot 'minebackup-cli.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE.txt') -Destination $packageRoot
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'LICENSES') -Force | Out-Null
foreach ($license in @('7zip-zstd.txt', 'spdlog.txt', 'fmt.txt', 'knotlink-sdk-cpp.txt')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSES\$license") `
        -Destination (Join-Path $packageRoot "LICENSES\$license")
}
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'examples') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\cli\server-manifest.example.json') `
    -Destination (Join-Path $packageRoot 'examples\server-manifest.json')
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'scheduling\systemd') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'scheduling\windows') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\cli\systemd\minebackup-backup@.service') `
    -Destination (Join-Path $packageRoot 'scheduling\systemd')
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\cli\systemd\minebackup-backup@.timer') `
    -Destination (Join-Path $packageRoot 'scheduling\systemd')
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\cli\systemd\example.env') `
    -Destination (Join-Path $packageRoot 'scheduling\systemd')
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\cli\windows\MineBackup-Job.xml') `
    -Destination (Join-Path $packageRoot 'scheduling\windows')

New-Item -ItemType Directory -Path $output -Force | Out-Null
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive -CompressionLevel Optimal
$verify = Join-Path $stage 'verify'
Expand-Archive -LiteralPath $archive -DestinationPath $verify
$verifiedBinary = Join-Path $verify "MineBackup-CLI-$Version-windows-x64\minebackup-cli.exe"
if (-not (Test-Path -LiteralPath $verifiedBinary -PathType Leaf)) {
    throw 'Packaged minebackup-cli.exe is missing.'
}
& $verifiedBinary --version
if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI smoke test failed.' }
Remove-Item -LiteralPath $stage -Recurse -Force
Get-FileHash -LiteralPath $archive -Algorithm SHA256

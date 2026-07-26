[CmdletBinding()]
param(
    [string]$TargetPath
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($TargetPath)) {
    $TargetPath = [System.IO.Path]::Combine($PSScriptRoot, '..\MineBackup\Assets\7za.exe')
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $bytes = $algorithm.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

$expectedSha256 = 'd8d92a77b6b34b07deafdac6e0845076eac5bf76bf026b554d101e81363cd052'
$target = [System.IO.Path]::GetFullPath($TargetPath)
if (-not [System.IO.File]::Exists($target)) {
    throw "The tracked Windows 7-Zip resource is missing: $target"
}

$actualSha256 = Get-Sha256Hex -Path $target
if ($actualSha256 -ne $expectedSha256) {
    throw "Tracked 7za.exe hash mismatch. Expected $expectedSha256, got $actualSha256"
}

Write-Host "Verified tracked 7za.exe: $target"

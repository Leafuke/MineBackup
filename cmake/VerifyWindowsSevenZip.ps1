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

$expectedSha256 = '051afc5dce51d2c8802d81a44ed5433a8d31fb158d9e9eb0a37e75b3b81fd867'
$target = [System.IO.Path]::GetFullPath($TargetPath)
if (-not [System.IO.File]::Exists($target)) {
    throw "The tracked Windows 7-Zip resource is missing: $target"
}

$actualSha256 = Get-Sha256Hex -Path $target
if ($actualSha256 -ne $expectedSha256) {
    throw "Tracked 7za.exe hash mismatch. Expected $expectedSha256, got $actualSha256"
}

Write-Host "Verified tracked 7za.exe: $target"

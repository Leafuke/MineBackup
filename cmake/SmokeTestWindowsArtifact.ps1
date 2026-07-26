param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [ValidateRange(2, 30)]
    [int]$StartupSeconds = 5
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) (
    'minebackup-windows-smoke-' + [Guid]::NewGuid().ToString('N'))
$profile = Join-Path $scratch 'profile'
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

try {
    $deprecated = Start-Process -FilePath $resolved -ArgumentList @('--service') `
        -WorkingDirectory $scratch -WindowStyle Hidden -PassThru
    try {
        if (-not $deprecated.WaitForExit(10000)) {
            Stop-Process -Id $deprecated.Id -Force
            $deprecated.WaitForExit(5000) | Out-Null
            throw 'Deprecated --service mode did not refuse startup within 10 seconds.'
        }
        if ($deprecated.ExitCode -ne 6) {
            throw "Deprecated --service mode exited with $($deprecated.ExitCode), expected 6."
        }
    }
    finally {
        if (-not $deprecated.HasExited) {
            Stop-Process -Id $deprecated.Id -Force
            $deprecated.WaitForExit(5000) | Out-Null
        }
        $deprecated.Dispose()
    }

    $arguments = @('--data-dir', ('"' + $profile + '"'), '--silent-startup')
    $process = Start-Process -FilePath $resolved -ArgumentList $arguments `
        -WorkingDirectory $scratch -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit($StartupSeconds * 1000)) {
            Write-Host "MineBackup remained running for the $StartupSeconds-second startup smoke."
        }
        else {
            throw "MineBackup exited during startup smoke with code $($process.ExitCode)."
        }
    }
    finally {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit(5000) | Out-Null
        }
        $process.Dispose()
    }
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Verified Windows startup and Service Mode refusal: $resolved"

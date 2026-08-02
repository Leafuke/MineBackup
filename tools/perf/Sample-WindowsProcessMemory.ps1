[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$ProcessId,

    [ValidateRange(1, 1000)]
    [int]$Samples = 5,

    [ValidateRange(100, 60000)]
    [int]$IntervalMilliseconds = 1000,

    [string]$Label = "sample",

    [string]$CsvPath
)

$ErrorActionPreference = "Stop"

function Get-Median {
    param([double[]]$Values)

    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return $ordered[$middle]
    }

    return ($ordered[$middle - 1] + $ordered[$middle]) / 2.0
}

function Convert-BytesToMiB {
    param([UInt64]$Bytes)
    return [Math]::Round($Bytes / 1MB, 2)
}

$rows = @()
for ($sampleIndex = 1; $sampleIndex -le $Samples; $sampleIndex++) {
    $process = Get-CimInstance -ClassName Win32_PerfFormattedData_PerfProc_Process `
        -Filter "IDProcess = $ProcessId"
    if ($null -eq $process) {
        throw "Process $ProcessId is not running or has no performance-counter data."
    }

    $rows += [pscustomobject]@{
        Label                 = $Label
        Sample                = $sampleIndex
        Timestamp             = (Get-Date).ToString("o")
        ProcessId             = $ProcessId
        PrivateBytesMiB       = Convert-BytesToMiB $process.PrivateBytes
        PrivateWorkingSetMiB  = Convert-BytesToMiB $process.WorkingSetPrivate
        WorkingSetMiB         = Convert-BytesToMiB $process.WorkingSet
        Threads               = [int]$process.ThreadCount
        Handles               = [int]$process.HandleCount
    }

    if ($sampleIndex -lt $Samples) {
        Start-Sleep -Milliseconds $IntervalMilliseconds
    }
}

if ($CsvPath) {
    $parent = Split-Path -Parent $CsvPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding utf8
}

$rows | Format-Table -AutoSize

[pscustomobject]@{
    Label                        = $Label
    Samples                      = $Samples
    MedianPrivateBytesMiB        = [Math]::Round((Get-Median $rows.PrivateBytesMiB), 2)
    MedianPrivateWorkingSetMiB   = [Math]::Round((Get-Median $rows.PrivateWorkingSetMiB), 2)
    MedianWorkingSetMiB          = [Math]::Round((Get-Median $rows.WorkingSetMiB), 2)
    MedianThreads                = [Math]::Round((Get-Median $rows.Threads), 1)
    MedianHandles                = [Math]::Round((Get-Median $rows.Handles), 1)
} | Format-List

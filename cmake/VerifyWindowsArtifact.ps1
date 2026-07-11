param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$version = (Get-Item -LiteralPath $resolved).VersionInfo
if ($version.FileVersion -ne '1.16.0' -or $version.ProductVersion -ne '1.16.0') {
    throw "Unexpected version metadata: file=$($version.FileVersion), product=$($version.ProductVersion)"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found'
}
$dumpbin = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' |
    Select-Object -First 1
if (-not $dumpbin) {
    throw 'dumpbin.exe was not found'
}

$headers = & $dumpbin /headers $resolved | Out-String
if ($headers -notmatch 'machine \(x64\)' -or $headers -notmatch 'subsystem \(Windows GUI\)') {
    throw 'Executable is not an x64 Windows GUI image'
}
if ($headers -notmatch '\.rsrc') {
    throw 'Executable has no resource section'
}

$dependencies = & $dumpbin /dependents $resolved | Out-String
$forbidden = @('MSVCP\d*\.dll', 'VCRUNTIME\d*(?:_\d+)?\.dll', 'ucrtbase\.dll', 'api-ms-win-crt-[^\s]+\.dll')
foreach ($pattern in $forbidden) {
    if ($dependencies -match $pattern) {
        throw "Executable dynamically depends on the VC runtime: $($Matches[0])"
    }
}

Write-Host "Verified Windows artifact: $resolved"

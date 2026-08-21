param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dumpbin) {
    $dumpbinPath = $dumpbin.Source
}
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'dumpbin.exe and vswhere.exe were not found.'
    }
    $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) {
        throw 'No Visual Studio C++ tool installation was found.'
    }
    $dumpbinPath = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $dumpbinPath) {
        throw 'dumpbin.exe was not found in the Visual Studio C++ tool installation.'
    }
}

$headers = (& $dumpbinPath /headers $resolvedExecutable 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /headers failed:`n$headers"
}
if ($headers -notmatch '(?im)^\s*[0-9a-f]+\s+subsystem\s+\(Windows CUI\)\s*$') {
    throw "minebackup-cli is not a Windows console-subsystem executable:`n$headers"
}

$dependents = (& $dumpbinPath /dependents $resolvedExecutable 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /dependents failed:`n$dependents"
}
$forbidden = @('OPENGL32.dll', 'GLFW', 'USER32.dll', 'GDI32.dll')
foreach ($library in $forbidden) {
    if ($dependents -match [regex]::Escape($library)) {
        throw "minebackup-cli imports forbidden desktop dependency '$library':`n$dependents"
    }
}

Write-Host "Windows console subsystem and dependency audit passed."
Write-Host $dependents

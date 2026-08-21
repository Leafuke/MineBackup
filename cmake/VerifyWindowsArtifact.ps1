param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$version = (Get-Item -LiteralPath $resolved).VersionInfo
if ($version.FileVersion -ne '1.16.2' -or $version.ProductVersion -ne '1.16.2') {
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

if (-not ('MineBackupResourceProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class MineBackupResourceProbe
{
    private const uint LOAD_LIBRARY_AS_DATAFILE = 0x00000002;
    private const uint LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x00000020;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);

    [DllImport("kernel32.dll", EntryPoint = "FindResourceW", SetLastError = true)]
    private static extern IntPtr FindResourceById(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll", EntryPoint = "FindResourceW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr FindResourceByName(IntPtr module, IntPtr name, string type);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SizeofResource(IntPtr module, IntPtr resource);

    [DllImport("kernel32.dll")]
    private static extern bool FreeLibrary(IntPtr module);

    private static IntPtr ResourceId(ushort value)
    {
        return new IntPtr(value);
    }

    public static uint SizeOf(string path, ushort id, ushort type)
    {
        return SizeOfCore(path, module => FindResourceById(module, ResourceId(id), ResourceId(type)));
    }

    public static uint SizeOf(string path, ushort id, string type)
    {
        return SizeOfCore(path, module => FindResourceByName(module, ResourceId(id), type));
    }

    private static uint SizeOfCore(string path, Func<IntPtr, IntPtr> find)
    {
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            IntPtr resource = find(module);
            return resource == IntPtr.Zero ? 0 : SizeofResource(module, resource);
        }
        finally
        {
            FreeLibrary(module);
        }
    }
}
'@
}

$requiredResources = @(
    @{ Label = 'primary icon'; Size = [MineBackupResourceProbe]::SizeOf($resolved, [System.UInt16]102, [System.UInt16]14) },
    @{ Label = 'alternate icon'; Size = [MineBackupResourceProbe]::SizeOf($resolved, [System.UInt16]104, [System.UInt16]14) },
    @{ Label = 'embedded 7-Zip'; Size = [MineBackupResourceProbe]::SizeOf($resolved, [System.UInt16]101, 'EXE') },
    @{ Label = 'embedded icon font'; Size = [MineBackupResourceProbe]::SizeOf($resolved, [System.UInt16]111, 'FONTS') }
)
foreach ($resource in $requiredResources) {
    if ($resource.Size -le 0) {
        throw "Executable is missing its $($resource.Label) resource"
    }
}

$dependencies = & $dumpbin /dependents $resolved | Out-String
$forbidden = @('MSVCP\d*\.dll', 'VCRUNTIME\d*(?:_\d+)?\.dll', 'ucrtbase\.dll', 'api-ms-win-crt-[^\s]+\.dll')
foreach ($pattern in $forbidden) {
    if ($dependencies -match $pattern) {
        throw "Executable dynamically depends on the VC runtime: $($Matches[0])"
    }
}

Write-Host "Verified Windows artifact: $resolved"

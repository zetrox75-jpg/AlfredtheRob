# update_menu.ps1 - replace the menu embedded in RobRunPort.exe.
#
# The overlay loads its menu from resource 101 (RCDATA) inside its own exe, not
# from a loose file. build_port.bat cannot be used here because rr_main.c is not
# part of this source snapshot, so this script updates the resource in place.
#
#   1. Edit menu.html.
#   2. Run:  .\update_menu.ps1 -Rebuild
#   3. The launcher (RobRunAIO.exe) is rebuilt so it carries the new overlay.
#
# A copy of the unpatched overlay is kept as RobRunPort.exe.orig the first time.

[CmdletBinding()]
param(
    [string]$Overlay = (Join-Path $PSScriptRoot '..\1_run\RobRunPort.exe'),
    [string]$Html    = (Join-Path $PSScriptRoot 'menu.html'),
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ResUpdater
{
    const uint LOAD_LIBRARY_AS_DATAFILE = 0x2;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryExW(string lpFileName, IntPtr hFile, uint dwFlags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FreeLibrary(IntPtr hModule);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr FindResourceW(IntPtr hModule, IntPtr lpName, IntPtr lpType);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LoadResource(IntPtr hModule, IntPtr hResInfo);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LockResource(IntPtr hResData);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint SizeofResource(IntPtr hModule, IntPtr hResInfo);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr BeginUpdateResourceW(string pFileName, bool bDeleteExistingResources);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResourceW(IntPtr hUpdate, IntPtr lpType, IntPtr lpName, ushort wLanguage, byte[] lpData, uint cbData);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EndUpdateResourceW(IntPtr hUpdate, bool fDiscard);
    delegate bool EnumResLangProc(IntPtr hModule, IntPtr lpType, IntPtr lpName, ushort wIDLanguage, IntPtr lParam);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EnumResourceLanguagesW(IntPtr hModule, IntPtr lpType, IntPtr lpName, EnumResLangProc lpEnumFunc, IntPtr lParam);

    public static ushort FindLanguage(string path, int type, int name)
    {
        IntPtr h = LoadLibraryExW(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (h == IntPtr.Zero) return 0;
        ushort found = 0;
        EnumResourceLanguagesW(h, (IntPtr)type, (IntPtr)name,
            delegate(IntPtr hm, IntPtr t, IntPtr n, ushort lang, IntPtr lp) { found = lang; return false; },
            IntPtr.Zero);
        FreeLibrary(h);
        return found;
    }

    public static byte[] Read(string path, int type, int name)
    {
        IntPtr h = LoadLibraryExW(path, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (h == IntPtr.Zero) return null;
        IntPtr info = FindResourceW(h, (IntPtr)name, (IntPtr)type);
        if (info == IntPtr.Zero) { FreeLibrary(h); return null; }
        uint size = SizeofResource(h, info);
        IntPtr data = LockResource(LoadResource(h, info));
        byte[] buf = new byte[size];
        Marshal.Copy(data, buf, 0, (int)size);
        FreeLibrary(h);
        return buf;
    }

    public static void Patch(string path, int type, int name, ushort lang, byte[] data)
    {
        IntPtr h = BeginUpdateResourceW(path, false);
        if (h == IntPtr.Zero) throw new Exception("BeginUpdateResource failed: " + Marshal.GetLastWin32Error());
        if (!UpdateResourceW(h, (IntPtr)type, (IntPtr)name, lang, data, (uint)data.Length))
        {
            int err = Marshal.GetLastWin32Error();
            EndUpdateResourceW(h, true);
            throw new Exception("UpdateResource failed: " + err);
        }
        if (!EndUpdateResourceW(h, false))
            throw new Exception("EndUpdateResource failed: " + Marshal.GetLastWin32Error());
    }
}
'@ -Language CSharp

$Overlay = (Resolve-Path -LiteralPath $Overlay).Path
$Html    = (Resolve-Path -LiteralPath $Html).Path
$bytes   = [IO.File]::ReadAllBytes($Html)
$lang    = [ResUpdater]::FindLanguage($Overlay, 10, 101)
if ($lang -eq 0) { $lang = 1033 }

$backup = "$Overlay.orig"
if (-not (Test-Path -LiteralPath $backup)) {
    Copy-Item -LiteralPath $Overlay -Destination $backup
    Write-Host "backup: $backup"
}

Write-Host "patching resource 101 (lang $lang) into $Overlay"
[ResUpdater]::Patch($Overlay, 10, 101, $lang, $bytes)

$after = [ResUpdater]::Read($Overlay, 10, 101)
$ok = $after -and $after.Length -eq $bytes.Length
if ($ok) {
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($after[$i] -ne $bytes[$i]) { $ok = $false; break }
    }
}
if (-not $ok) {
    Copy-Item -LiteralPath $backup -Destination $Overlay -Force
    throw "verification failed; overlay restored from $backup"
}
Write-Host "menu resource updated and verified ($($bytes.Length) bytes)"

if ($Rebuild) {
    Push-Location $PSScriptRoot
    try { & cmd /c 'build_aio.bat' } finally { Pop-Location }
}

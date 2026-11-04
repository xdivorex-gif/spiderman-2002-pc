<#
  Native call stacks of every thread of a running spiderman_recomp.exe, named
  from build\Release\spiderman_recomp.map -- for a game that has frozen.

      powershell -ExecutionPolicy Bypass -File tools\stacks.ps1 [-Id <pid>] [-Frames 24]

  Each thread is suspended for the moment it is read, then resumed. Uses
  dbghelp's StackWalk64 with the exe's own unwind data, so no PDB is needed;
  the map file must be the one the running exe was linked with.
#>
param([int]$Id = 0, [int]$Frames = 24)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Stacks
{
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, int pid);
    [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint a, bool i, int tid);
    [DllImport("kernel32.dll")] static extern int SuspendThread(IntPtr t);
    [DllImport("kernel32.dll")] static extern int ResumeThread(IntPtr t);
    [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr t, IntPtr ctx);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string n);
    [DllImport("kernel32.dll")] static extern IntPtr LoadLibrary(string n);
    [DllImport("kernel32.dll")] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("dbghelp.dll")] static extern bool SymInitialize(IntPtr p, string path, bool invade);
    [DllImport("dbghelp.dll")] static extern bool SymCleanup(IntPtr p);
    [DllImport("dbghelp.dll")] static extern bool StackWalk64(uint machine, IntPtr p, IntPtr t, IntPtr frame,
        IntPtr ctx, IntPtr read, IntPtr fta, IntPtr gmb, IntPtr tr);

    public static List<string> Walk(int pid, int[] tids, int maxFrames)
    {
        var o = new List<string>();
        IntPtr p = OpenProcess(0x1F0FFF, false, pid);
        if (p == IntPtr.Zero) { o.Add("cannot open process"); return o; }
        SymInitialize(p, null, true);
        IntPtr dbg = LoadLibrary("dbghelp.dll");
        IntPtr fta = GetProcAddress(dbg, "SymFunctionTableAccess64");
        IntPtr gmb = GetProcAddress(dbg, "SymGetModuleBase64");
        IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
        IntPtr ctx = new IntPtr((raw.ToInt64() + 15) & ~15L);
        IntPtr frame = Marshal.AllocHGlobal(1024);
        foreach (int tid in tids) {
            IntPtr t = OpenThread(0x1FFFFF, false, tid);
            if (t == IntPtr.Zero) { o.Add("T " + tid + " cannot open"); continue; }
            SuspendThread(t);
            for (int i = 0; i < 1232; i++) Marshal.WriteByte(ctx, i, 0);
            Marshal.WriteInt32(ctx, 0x30, 0x10000B);           /* CONTEXT_FULL, AMD64 */
            var line = new System.Text.StringBuilder("T " + tid);
            if (GetThreadContext(t, ctx)) {
                for (int i = 0; i < 1024; i++) Marshal.WriteByte(frame, i, 0);
                Marshal.WriteInt64(frame, 0x00, Marshal.ReadInt64(ctx, 0xF8));   /* AddrPC = Rip */
                Marshal.WriteInt32(frame, 0x0C, 3);
                Marshal.WriteInt64(frame, 0x20, Marshal.ReadInt64(ctx, 0xA0));   /* AddrFrame = Rbp */
                Marshal.WriteInt32(frame, 0x2C, 3);
                Marshal.WriteInt64(frame, 0x30, Marshal.ReadInt64(ctx, 0x98));   /* AddrStack = Rsp */
                Marshal.WriteInt32(frame, 0x3C, 3);
                for (int n = 0; n < maxFrames; n++) {
                    if (!StackWalk64(0x8664, p, t, frame, ctx, IntPtr.Zero, fta, gmb, IntPtr.Zero)) break;
                    long pc = Marshal.ReadInt64(frame, 0);
                    if (pc == 0) break;
                    line.Append(" ").Append(pc.ToString("X"));
                }
            }
            ResumeThread(t);
            CloseHandle(t);
            o.Add(line.ToString());
        }
        SymCleanup(p);
        Marshal.FreeHGlobal(raw);
        Marshal.FreeHGlobal(frame);
        CloseHandle(p);
        return o;
    }
}
'@

$proc = if ($Id) { Get-Process -Id $Id } else { Get-Process spiderman_recomp | Select-Object -First 1 }
$exe = $proc.MainModule
$base = $exe.BaseAddress.ToInt64()
$map = [IO.Path]::ChangeExtension($exe.FileName, '.map')
if ($exe.FileName -like '*running.exe') { $map = $map -replace '\.running\.map$', '.map' }

# Map: "<sec>:<off>  <name>  <rva+base>  f  <obj>"; the preferred base is in
# the header line "Preferred load address is <hex>".
$pref = 0; $syms = New-Object 'System.Collections.Generic.List[object]'
foreach ($l in [IO.File]::ReadLines($map)) {
    if ($l -match 'Preferred load address is ([0-9a-fA-F]+)') { $pref = [Convert]::ToInt64($Matches[1], 16); continue }
    if ($l -match '^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s') {
        $syms.Add([pscustomobject]@{ a = [Convert]::ToInt64($Matches[2], 16) - $pref; n = $Matches[1] })
    }
}
$sorted = $syms | Sort-Object a
$addrs = [long[]]($sorted | ForEach-Object { $_.a }); $names = [string[]]($sorted | ForEach-Object { $_.n })
function Name([long]$pc) {
    $rva = $pc - $base
    if ($rva -lt 0 -or $rva -gt 0x7FFFFFFF) { return ('{0:X}' -f $pc) }
    $i = [Array]::BinarySearch($addrs, $rva); if ($i -lt 0) { $i = (-bnot $i) - 1 }
    if ($i -lt 0) { return ('{0:X}' -f $pc) }
    return ('{0}+{1:X}' -f $names[$i], ($rva - $addrs[$i]))
}

"process $($proc.Id) $($exe.FileName), $($syms.Count) symbols from $map"
$tids = [int[]]($proc.Threads | ForEach-Object { $_.Id })
foreach ($row in [Stacks]::Walk($proc.Id, $tids, $Frames)) {
    $parts = $row -split ' '
    "-- thread $($parts[1])"
    foreach ($h in ($parts | Select-Object -Skip 2)) { '     ' + (Name ([Convert]::ToInt64($h, 16))) }
}

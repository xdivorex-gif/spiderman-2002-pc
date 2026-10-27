# Run spiderman_recomp.exe for a while and capture its logs.
#
#   .\run.ps1                                  # 60 s, no extra switches
#   .\run.ps1 -Seconds 30 -Env @{ RECOMP_PB_EXEC = "1" }
#
# Output goes to run_out.txt (the game's own prints) and run_err.txt
# (toolkit diagnostics).
#
# The watchdog is a timer, not a hang detector: after -Watchdog seconds it
# dumps the guest stack and ends the process, running or not. By default it
# fires 3 s before -Seconds, so every run ends with a snapshot of where the
# game was. -Watchdog 0 turns it off.
param(
    [int]$Seconds = 60,
    [int]$Watchdog = -1,
    [hashtable]$Env = @{}
)
if ($Watchdog -lt 0) { $Watchdog = [Math]::Max(5, $Seconds - 3) }

Set-Location $PSScriptRoot
# A run whose exit hangs keeps the exe locked and the next link fails
# (LNK1104). Clear any leftover first, and again at the end.
Get-Process spiderman_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
if ($Watchdog -gt 0) { $env:RECOMP_WATCHDOG_SECS = "$Watchdog" }
else { Remove-Item env:RECOMP_WATCHDOG_SECS -ErrorAction SilentlyContinue }
$env:RECOMP_STUB_LOG = "1"
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }

$p = Start-Process -FilePath build\Release\spiderman_recomp.exe `
    -WorkingDirectory $PSScriptRoot `
    -RedirectStandardOutput run_out.txt -RedirectStandardError run_err.txt `
    -PassThru
$handle = $p.Handle  # keeps ExitCode readable after exit
if ($p.WaitForExit($Seconds * 1000)) {
    "exited after {0:N0}s, code 0x{1:X8}" -f ($p.ExitTime - $p.StartTime).TotalSeconds, $p.ExitCode
} else {
    Stop-Process -Id $p.Id -Force
    "still running after ${Seconds}s - stopped"
}
Get-Process spiderman_recomp -ErrorAction SilentlyContinue | Stop-Process -Force

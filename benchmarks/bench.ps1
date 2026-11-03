# Mechano Reader benchmark (brief section 30). Read-only: never changes the test archives.
#   powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\bench.ps1 [-Runs 5]
# Measures: startup time + RAM at idle, opening a 100,000-entry ZIP (time + RAM),
# opening the preview test archive. Targets from docs/architecture/ARCHITECTURE.md.
param([int]$Runs = 5, [string]$Exe = "$PSScriptRoot\..\build\x64\Release\MechanoZip.exe")
$ErrorActionPreference = 'Stop'
Get-ChildItem env: | Where-Object { $_.Value.Length -gt 2000 } | ForEach-Object { Remove-Item "env:$($_.Name)" }
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class B {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
}
"@
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$Exe = (Resolve-Path $Exe).Path
$log = "$env:LOCALAPPDATA\MechanoZip\mechanozip.log"   # the app logs "Startup: window shown at N ms" (from process creation)
Get-Process MechanoZip -ErrorAction SilentlyContinue | Stop-Process -Force

function MainOf($proc) {
  $h = [IntPtr]::Zero
  do {
    $h = [B]::FindWindowExW([IntPtr]::Zero, $h, "MechanoZipReader.Main", [NullString]::Value)
    if ($h -ne [IntPtr]::Zero) { $pid2 = 0; [B]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null; if ($pid2 -eq $proc.Id) { return $h } }
  } while ($h -ne [IntPtr]::Zero)
  return [IntPtr]::Zero
}
function Mem($proc) { $proc.Refresh(); "{0,6:N1} MB private, {1,6:N1} MB working set" -f ($proc.PrivateMemorySize64 / 1MB), ($proc.WorkingSet64 / 1MB) }
function Median($a) { $s = $a | Sort-Object; "{0} (best {1})" -f $s[[int][math]::Floor($s.Count / 2)], $s[0] }

# Opens the app (optionally with an archive) and waits until the list shows $expect rows.
function Run([string]$arg, [int]$expect) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = if ($arg) { Start-Process $Exe -ArgumentList "`"$arg`"" -PassThru } else { Start-Process $Exe -PassThru }
  $null = $p.WaitForInputIdle(20000)
  $main = [IntPtr]::Zero
  while ($main -eq [IntPtr]::Zero -and $sw.ElapsedMilliseconds -lt 20000) { $main = MainOf $p; if ($main -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 5 } }
  $shown = $sw.ElapsedMilliseconds
  if ($expect -gt 0) {
    $list = [B]::FindWindowExW($main, [IntPtr]::Zero, "SysListView32", [NullString]::Value)
    while ([int][B]::SendMessageW($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) -lt $expect -and $sw.ElapsedMilliseconds -lt 60000) { Start-Sleep -Milliseconds 5 }
  }
  $ready = $sw.ElapsedMilliseconds
  Start-Sleep 1
  $m = Mem $p
  Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 300
  $vis = ((Get-Content $log -Encoding utf8 | Select-String "Startup: window shown at (\d+) ms" | Select-Object -Last 1).Matches[0].Groups[1].Value)
  [pscustomobject]@{ Visible = [int]$vis; Shown = $shown; Ready = $ready; Mem = $m }
}

# 100,000 small files in the archive root (generated once, in %TEMP%).
$big = Join-Path $env:TEMP "mzr-bench\big-100k.zip"
if (-not (Test-Path $big)) {
  New-Item -ItemType Directory (Split-Path $big) -Force | Out-Null
  "Generating $big ..."
  $z = [IO.Compression.ZipFile]::Open($big, 'Create')
  $bytes = [Text.Encoding]::UTF8.GetBytes("benchmark file content`r`n" * 4)
  for ($i = 0; $i -lt 100000; $i++) {
    $e = $z.CreateEntry(("file_{0:D6}.txt" -f $i)); $s = $e.Open(); $s.Write($bytes, 0, $bytes.Length); $s.Dispose()
  }
  $z.Dispose()
}

"Mechano Reader benchmark   exe: $([IO.FileInfo]::new($Exe).Length / 1KB) KB   $(Get-Date -Format s)"
"Runs: $Runs (median)"
$r = 1..$Runs | ForEach-Object { Run "" 0 }
"Startup: window visible          : {0,-16} ms   target < 200 ms   (app log, from process creation)" -f (Median $r.Visible)
"Startup: ready for input         : {0,-16} ms   (incl. work done right after the window shows)" -f (Median $r.Shown)
"RAM idle                         : {0}   (target <= 15 MB)" -f $r[-1].Mem
$r = 1..$Runs | ForEach-Object { Run $big 100000 }
"Open 100,000-entry ZIP (incl. start): {0,-16} ms                       RAM: {1}   (target <= 40 MB)" -f (Median $r.Ready), $r[-1].Mem
$sample = Resolve-Path "$PSScriptRoot\..\..\00-data\test-archives\sample-preview.zip"
$r = 1..$Runs | ForEach-Object { Run $sample 1 }
"Open sample-preview.zip (incl. start): {0,-16} ms                      RAM: {1}" -f (Median $r.Ready), $r[-1].Mem

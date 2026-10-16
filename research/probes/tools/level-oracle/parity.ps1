# Collision parity (openBF3 TC, docs/design/terrain-collision.md §6, gate G2): `bf3dump collision` against
# `level-oracle collision` for every mode of every one of the 29 MP levels, byte for byte. Research tool, not shipped.
#
#   pwsh research/probes/tools/level-oracle/parity.ps1 -Out <scratch> [-Bf3dump <exe>] [-Oracle <exe>] [-Jobs N]
#                                                     [-Levels MP_Subway,XP3_Alborz] [-IW4L <checkout>]
#
# First line: the IW4L commit the oracle builds against and whether its tree is dirty. Without -Oracle the script
# builds the oracle (cargo, release) with CARGO_TARGET_DIR=<Out>\target. Modes come from `bf3dump levelmodes`. Each
# run writes <Out>\<level>.<mode>.{cpp,rust}.txt (and .err); the listings are compared byte for byte and the run
# prints SAME or DIFF with both tools' wall times. Ends with the totals; exits 1 on any DIFF or failed run.
# Everything the tools read comes from the install read-only; the outputs hold decoded BF3 data, so <Out> must be a
# scratch folder outside the repository.
param(
	[Parameter(Mandatory = $true)][string]$Out,
	[string]$Bf3dump = '',
	[string]$Oracle = '',
	[string]$IW4L = 'C:\Users\olku\iw4L',
	[string[]]$Levels = @(),
	[int]$Jobs = 1
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if ($Bf3dump -eq '') { $Bf3dump = Join-Path $repo 'b\dev\tools\bf3dump\bf3dump.exe' }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$commit = (git -C $IW4L rev-parse HEAD 2>$null)
$dirty = if ((git -C $IW4L status --short 2>$null | Measure-Object).Count -gt 0) { 'dirty' } else { 'clean' }
Write-Output "IW4L $IW4L commit $commit ($dirty)"

if ($Oracle -eq '') {
	$env:CARGO_TARGET_DIR = Join-Path $Out 'target'
	cargo build --release --quiet --manifest-path (Join-Path $PSScriptRoot 'Cargo.toml')
	if ($LASTEXITCODE -ne 0) { Write-Output 'oracle build failed'; exit 1 }
	$Oracle = Join-Path $env:CARGO_TARGET_DIR 'release\level-oracle.exe'
}
if (-not (Test-Path $Bf3dump)) { Write-Output "no bf3dump at $Bf3dump"; exit 1 }
if (-not (Test-Path $Oracle)) { Write-Output "no level-oracle at $Oracle"; exit 1 }

$all = @('MP_001', 'MP_003', 'MP_007', 'MP_011', 'MP_012', 'MP_013', 'MP_017', 'MP_018', 'MP_Subway', 'XP1_001', 'XP1_002', 'XP1_003',
	'XP1_004', 'XP2_Factory', 'XP2_Office', 'XP2_Palace', 'XP2_Skybar', 'XP3_Alborz', 'XP3_Desert', 'XP3_Shield', 'XP3_Valley', 'XP4_FD',
	'XP4_Parl', 'XP4_Quake', 'XP4_Rubble', 'XP5_001', 'XP5_002', 'XP5_003', 'XP5_004')
if ($Levels.Count -eq 0) { $Levels = $all }

$runs = foreach ($level in $Levels) {
	$modes = & $Bf3dump levelmodes $level
	if ($LASTEXITCODE -ne 0) { Write-Output "levelmodes $level failed"; exit 1 }
	foreach ($mode in $modes) { if ($mode.Trim() -ne '') { [pscustomobject]@{ Level = $level; Mode = $mode.Trim() } } }
}

$start = Get-Date
# cmd does the redirection, so the listings keep their bytes (PowerShell's > would re-encode them).
$results = $runs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
	$level = $_.Level
	$mode = $_.Mode
	$base = Join-Path $using:Out ($level + '.' + $mode)
	$timed = {
		param($exe, $file, $lv, $md)
		$t = Get-Date
		cmd /c "`"$exe`" collision $lv $md > `"$file.txt`" 2> `"$file.err`""
		[pscustomobject]@{ Code = $LASTEXITCODE; Seconds = ((Get-Date) - $t).TotalSeconds }
	}
	$cpp = & $timed $using:Bf3dump ($base + '.cpp') $level $mode
	$rust = & $timed $using:Oracle ($base + '.rust') $level $mode
	$same = $false
	if ($cpp.Code -eq 0 -and $rust.Code -eq 0) {
		$same = (Get-FileHash ($base + '.cpp.txt')).Hash -eq (Get-FileHash ($base + '.rust.txt')).Hash
	}
	$total = Select-String -Path ($base + '.cpp.txt') -Pattern '^soup ' -SimpleMatch:$false | Select-Object -First 1
	[pscustomobject]@{
		Level = $level; Mode = $mode; Same = $same; CppCode = $cpp.Code; RustCode = $rust.Code
		Cpp = $cpp.Seconds; Rust = $rust.Seconds; Totals = if ($total) { $total.Line } else { '' }
	}
}
$results = @($results | Sort-Object Level, Mode)
foreach ($r in $results) {
	$verdict = if ($r.Same) { 'SAME' } elseif ($r.CppCode -ne 0 -or $r.RustCode -ne 0) { "FAIL (bf3dump $($r.CppCode), oracle $($r.RustCode))" } else { 'DIFF' }
	Write-Output ('{0,-5} {1} {2}  bf3dump {3:N2} s  oracle {4:N2} s  {5}' -f $verdict.Split(' ')[0], $r.Level, $r.Mode, $r.Cpp, $r.Rust, $r.Totals)
	if (-not $r.Same -and $verdict -ne 'DIFF') { Write-Output "      $verdict" }
}
$same = @($results | Where-Object Same).Count
$wall = ((Get-Date) - $start).TotalSeconds
$cppSum = ($results | Measure-Object Cpp -Sum).Sum
$rustSum = ($results | Measure-Object Rust -Sum).Sum
Write-Output ('parity: {0} / {1} runs identical; wall {2:N0} s (bf3dump {3:N0} s, oracle {4:N0} s summed; jobs {5}); IW4L {6} ({7})' -f $same,
	$results.Count, $wall, $cppSum, $rustSum, $Jobs, $commit, $dirty)
if ($same -ne $results.Count -or $results.Count -eq 0) { exit 1 }
exit 0

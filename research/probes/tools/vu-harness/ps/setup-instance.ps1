# Create or refresh the measurement instance (DESIGN.md 11.2). THE USER RUNS THIS SCRIPT.
#   pwsh -NoProfile -File setup-instance.ps1 [-Target <dir>] [-Session p0|s1|s2|s3] [-Password <pw>] [-NewPassword]
#        [-CopyKeyFrom <path>] [-DisablePreRound] [-WhatIf]
# Writes Admin\ModList.txt (exactly mharness), Admin\MapList.txt per session, the neutral Admin\Startup.txt, and
# mirrors mod\mharness into Admin\Mods\mharness (stale files are removed only inside that folder). Refuses the normal
# instance (<MyDocuments>\Battlefield 3\Server and C:\Users\olku\Documents\Battlefield 3\Server, and anything below),
# the BF3 install, %LOCALAPPDATA%\VeniceUnleashed and the repo, after full-path and link resolution; refuses a target
# that is or contains a reparse point. -CopyKeyFrom copies the named file's bytes to <target>\server.key and never
# prints them. Idempotent (a second run reports "no changes"); -WhatIf prints every intended write and writes nothing.
# -TestRefuse is for tests only: extra roots to refuse (stand-ins for the normal instance).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
	[string]$Target = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Battlefield 3\ServerMeasure'),
	[ValidateSet('p0', 's1', 's2', 's3')][string]$Session = 's1',
	[string]$Password,
	[switch]$NewPassword,
	[string]$CopyKeyFrom,
	[switch]$DisablePreRound,
	[string[]]$TestRefuse = @()
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mh-common.ps1')

function Fail([string]$msg, [int]$code = 2) {
	Write-Output "refused: $msg"
	exit $code
}

# --- refusals (before anything is read or written) ---------------------------------------------------------------
$repo = $PSScriptRoot
while ($repo -and -not (Test-Path -LiteralPath (Join-Path $repo '.git'))) {
	$up = Split-Path -Parent $repo
	if ($up -eq $repo) { $repo = $null } else { $repo = $up }
}
if (-not $repo) { $repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path }
$lad = [Environment]::GetFolderPath('LocalApplicationData')
$refuse = @(Get-MhNormalInstances) + @($script:MhInstallRoot, (Join-Path $lad 'VeniceUnleashed'), $repo) + $TestRefuse
$hit = Test-MhRefused $Target $refuse
if ($hit) { Fail "target $Target resolves inside $hit" }
$T = ConvertTo-MhFull $Target
$parentT = Split-Path -Parent $T
if (-not $parentT -or -not (Test-Path -LiteralPath $parentT -PathType Container)) { Fail "parent folder of $T does not exist" }
if (Test-Path -LiteralPath $T) {
	$ti = Get-Item -LiteralPath $T -Force
	if ($ti.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail "target $T is a junction or symlink" }
	foreach ($c in @(Get-ChildItem -LiteralPath $T -Recurse -Force -Attributes ReparsePoint -ErrorAction Stop)) {
		Fail "target contains a junction or symlink: $($c.FullName)"
	}
}
if ($CopyKeyFrom) {
	if (-not (Test-Path -LiteralPath $CopyKeyFrom -PathType Leaf)) { Fail "key file not found: $CopyKeyFrom" }
	$klen = (Get-Item -LiteralPath $CopyKeyFrom -Force).Length
	if ($klen -lt 1 -or $klen -gt 4096) { Fail "key file size $klen is not 1-4096 bytes" }
	if (Test-MhUnder (ConvertTo-MhFull $CopyKeyFrom) $T) { Fail 'the key source is inside the target' }
}
if ($Password -and $Password -notmatch '^[a-z0-9]{8,32}$') { Fail 'password must be 8-32 characters [a-z0-9]' }
$modSrc = ConvertTo-MhFull (Join-Path $PSScriptRoot '..\mod\mharness')
if (-not (Test-Path -LiteralPath (Join-Path $modSrc 'mod.json'))) { Fail "mod source not found: $modSrc" 1 }

# a running server on this target (read-only process query)
try {
	$procs = @(Get-CimInstance Win32_Process -Filter "Name='vu.exe'" -ErrorAction Stop | Where-Object {
			$_.CommandLine -and $_.CommandLine.IndexOf($T, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
	if ($procs.Count -gt 0) { Write-Output "warning: a vu.exe process uses $T; stop the server before changing it" }
} catch { }

# --- plan -----------------------------------------------------------------------------------------------------------
$utf8 = New-Object System.Text.UTF8Encoding($false)
$changes = 0
function Write-IfChanged([string]$path, [string]$text) {
	$bytes = $utf8.GetBytes($text)
	if (Test-Path -LiteralPath $path -PathType Leaf) {
		$old = [IO.File]::ReadAllBytes($path)
		if ([Linq.Enumerable]::SequenceEqual([byte[]]$old, [byte[]]$bytes)) { return }
	}
	if ($PSCmdlet.ShouldProcess($path, 'write')) {
		$d = Split-Path -Parent $path
		if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
		[IO.File]::WriteAllBytes($path, $bytes)
		Write-Output "wrote $path"
	}
	$script:changes++
}

$admin = Join-Path $T 'Admin'
$startup = Join-Path $admin 'Startup.txt'
$oldPw = $null
$oldDpr = $false
if (Test-Path -LiteralPath $startup -PathType Leaf) {
	foreach ($line in [IO.File]::ReadAllLines($startup)) {
		if ($line -match '^\s*vars\.gamePassword\s+"?([a-z0-9]{8,32})"?\s*$') { $oldPw = $Matches[1] }
		if ($line -match '^\s*vu\.DisablePreRound\s+true\s*$') { $oldDpr = $true }
	}
}
$pwShown = $false
if ($Password) { $pw = $Password }
elseif ($oldPw -and -not $NewPassword) { $pw = $oldPw }
else {
	$abc = 'abcdefghijklmnopqrstuvwxyz0123456789'
	$pw = -join (1..12 | ForEach-Object { $abc[[Security.Cryptography.RandomNumberGenerator]::GetInt32(0, 36)] })
	$pwShown = $true
}
$dpr = $DisablePreRound.IsPresent -or $oldDpr

$nl = "`r`n"
$maps = @{
	p0 = @('MP_Subway ConquestSmall0 1', 'MP_001 ConquestLarge0 1')
	s1 = @('MP_Subway ConquestSmall0 1', 'MP_001 ConquestLarge0 1')
	# Sessions 2-3: placeholders with the Session 1 levels until their scenarios exist (DESIGN 6.11)
	s2 = @('MP_Subway ConquestSmall0 1', 'MP_001 ConquestLarge0 1')
	s3 = @('MP_Subway ConquestSmall0 1', 'MP_001 ConquestLarge0 1')
}
$st = @(
	'vars.serverName "mh-measure"',
	('vars.gamePassword "{0}"' -f $pw),
	'vars.regenerateHealth true',
	'vars.soldierHealth 100',
	'vars.bulletDamage 100',
	'vars.playerRespawnTime 100',
	'vars.gameModeCounter 100',
	'vars.vehicleSpawnAllowed true',
	'vars.vehicleSpawnDelay 100',
	'vars.friendlyFire false',
	'vars.autoBalance false',
	'vars.idleTimeout 0',
	'vu.SuppressionMultiplier 100',
	'vu.TimeScale 1.0'
)
if ($dpr) { $st += 'vu.DisablePreRound true' }

if (-not (Test-Path -LiteralPath $T)) {
	if ($PSCmdlet.ShouldProcess($T, 'create folder')) { New-Item -ItemType Directory -Path $T | Out-Null; Write-Output "created $T" }
	$changes++
}
Write-IfChanged (Join-Path $admin 'ModList.txt') ('mharness' + $nl)
Write-IfChanged (Join-Path $admin 'MapList.txt') (($maps[$Session] -join $nl) + $nl)
Write-IfChanged $startup (($st -join $nl) + $nl)

# mirror mod\mharness -> Admin\Mods\mharness
$dst = Join-Path $admin 'Mods\mharness'
$srcFiles = @(Get-ChildItem -LiteralPath $modSrc -Recurse -File -Force | Where-Object {
		$_.FullName -notmatch '\\__pycache__\\' -and $_.Extension -ne '.pyc' })
$want = @{}
foreach ($f in $srcFiles) {
	$rel = $f.FullName.Substring($modSrc.Length).TrimStart('\')
	$want[$rel.ToLowerInvariant()] = $true
	$to = Join-Path $dst $rel
	$same = $false
	if (Test-Path -LiteralPath $to -PathType Leaf) {
		$a = [IO.File]::ReadAllBytes($f.FullName)
		$b = [IO.File]::ReadAllBytes($to)
		$same = [Linq.Enumerable]::SequenceEqual([byte[]]$a, [byte[]]$b)
	}
	if (-not $same) {
		if ($PSCmdlet.ShouldProcess($to, 'copy mod file')) {
			$d = Split-Path -Parent $to
			if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
			Copy-Item -LiteralPath $f.FullName -Destination $to -Force
		}
		$changes++
	}
}
if (Test-Path -LiteralPath $dst) {
	foreach ($g in @(Get-ChildItem -LiteralPath $dst -Recurse -File -Force)) {
		if ($g.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail "mod folder contains a link: $($g.FullName)" }
		$rel = $g.FullName.Substring((ConvertTo-MhFull $dst).Length).TrimStart('\')
		if (-not $want.ContainsKey($rel.ToLowerInvariant())) {
			if ($PSCmdlet.ShouldProcess($g.FullName, 'remove stale mod file')) {
				Remove-Item -LiteralPath $g.FullName -Force
				Write-Output "removed stale $($g.FullName)"
			}
			$changes++
		}
	}
}

if ($CopyKeyFrom) {
	$key = Join-Path $T 'server.key'
	if ($PSCmdlet.ShouldProcess($key, 'copy key bytes from the given file')) {
		Copy-Item -LiteralPath $CopyKeyFrom -Destination $key -Force
		Write-Output 'server.key copied (bytes not shown)'
	}
	$changes++
} elseif (-not (Test-Path -LiteralPath (Join-Path $T 'server.key'))) {
	Write-Output 'note: no server.key in the target; rerun with -CopyKeyFrom <your key file> or put a key there'
}

if ($pwShown -and -not $WhatIfPreference) { Write-Output "password: $pw" }
elseif ($Password -and $Password -ne $oldPw -and -not $WhatIfPreference) { Write-Output 'password: set from -Password' }
else { Write-Output 'password: kept (see Admin\Startup.txt)' }
if ($dpr) { Write-Output 'preround: disabled (vu.DisablePreRound true)' }
if ($changes -eq 0) { Write-Output 'no changes' } else { Write-Output ("{0} change(s){1}" -f $changes, $(if ($WhatIfPreference) { ' (WhatIf: nothing written)' } else { '' })) }
$vu = Join-Path $lad 'VeniceUnleashed\client\vu.exe'
Write-Output ('launch 30 Hz: & "{0}" -serverInstancePath "{1}" -server -dedicated -unlisted' -f $vu, $T)
Write-Output ('launch 60 Hz: & "{0}" -serverInstancePath "{1}" -server -dedicated -unlisted -high60' -f $vu, $T)
exit 0

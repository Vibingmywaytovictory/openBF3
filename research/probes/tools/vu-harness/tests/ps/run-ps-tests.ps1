# PowerShell tests (DESIGN.md 11, 14): plain asserts, no Pester.
#  * install-manifest / install-compare on a scratch tree: unchanged, then negative controls (touch, content change,
#    add, delete, header mismatch, empty) must exit 1; -Out inside the scratch "install", via "..", and via a junction
#    must be refused.
#  * setup-instance: -WhatIf writes nothing; a scratch target twice (second run "no changes"); refusals (normal
#    instance and its subfolder, lexical only, never touched; the install; a junction into a stand-in of the normal
#    instance; a target that contains a junction); a path that merely starts with "Server" is accepted (-WhatIf only);
#    a fake key copied byte-exact and never printed.
#  * -WithInstall: one read-only manifest of the real install with -Out in scratch, printing only its summary line.
# Scratch trees live under the system temp folder (or -Scratch) and are removed at the end.
[CmdletBinding()]
param([switch]$WithInstall, [string]$Scratch)
$ErrorActionPreference = 'Stop'
$PS = Join-Path $PSScriptRoot '..\..\ps'
$PS = (Resolve-Path $PS).Path
if (-not $Scratch) { $Scratch = Join-Path ([IO.Path]::GetTempPath()) ('mh-pstests-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)) }
New-Item -ItemType Directory -Path $Scratch -Force | Out-Null
$script:fails = 0
$script:n = 0

function Check([bool]$cond, [string]$what) {
	$script:n++
	if (-not $cond) { $script:fails++; Write-Output "FAIL $what" }
}
function Invoke-Ps([string]$script, [string[]]$a) {
	$out = & pwsh -NoProfile -File (Join-Path $PS $script) @a 2>&1 | ForEach-Object { "$_" }
	return [pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}

try {
	# ---------------- manifest / compare ----------------
	$inst = Join-Path $Scratch 'install'
	New-Item -ItemType Directory -Path (Join-Path $inst 'Data\Win32\levels') -Force | Out-Null
	New-Item -ItemType Directory -Path (Join-Path $inst 'Update\Patch') -Force | Out-Null
	$i = 0
	foreach ($ext in @('.toc', '.cat', '.mft', '.json', '.xml', '.exe', '.dll', '.cas', '.txt')) {
		$i++
		Set-Content -LiteralPath (Join-Path $inst ("Data\Win32\levels\f$i$ext")) -Value "content $i" -NoNewline
	}
	Set-Content -LiteralPath (Join-Path $inst 'Data\initfs_Win32') -Value 'initfs' -NoNewline
	Set-Content -LiteralPath (Join-Path $inst 'bf3.exe') -Value 'exe' -NoNewline
	Set-Content -LiteralPath (Join-Path $inst 'Update\Patch\p.cat') -Value 'patch' -NoNewline
	$outd = Join-Path $Scratch 'out'
	New-Item -ItemType Directory -Path $outd -Force | Out-Null
	$m1 = Join-Path $outd 'm1.csv'
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', $m1)
	Check ($r.Code -eq 0 -and $r.Out -match '^18 items -> .*\(10 hashed\)$') "manifest scratch: $($r.Out)"
	$csv = Import-Csv $m1
	Check (($csv | Where-Object { $_.Path -eq 'Data\Win32\levels\f8.cas' }).Sha256 -eq '') 'cas not hashed'
	Check (($csv | Where-Object { $_.Path -eq 'Data\initfs_Win32' }).Sha256.Length -eq 64) 'initfs hashed'
	Check (($csv | Where-Object { $_.Path -eq '.' }).Dir -eq 'True') 'root row'
	$m2 = Join-Path $outd 'm2.csv'
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', $m2)
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $m1, '-After', $m2)
	Check ($r.Code -eq 0 -and $r.Out -eq 'install unchanged: 18 items') "compare unchanged: $($r.Out)"

	# negative controls
	$f = Join-Path $inst 'Data\Win32\levels\f1.toc'
	(Get-Item $f).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddMinutes(5)
	$m3 = Join-Path $outd 'm3.csv'
	Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', $m3) | Out-Null
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $m1, '-After', $m3)
	Check ($r.Code -eq 1 -and $r.Out -match 'changed  Data\\Win32\\levels\\f1.toc \(LastWriteUtc\)') "touch detected: $($r.Out)"
	$f2 = Join-Path $inst 'Data\Win32\levels\f2.cat'
	$keep = (Get-Item $f2).LastWriteTimeUtc
	Set-Content -LiteralPath $f2 -Value 'content X' -NoNewline
	(Get-Item $f2).LastWriteTimeUtc = $keep
	Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', $m3) | Out-Null
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $m1, '-After', $m3)
	Check ($r.Code -eq 1 -and $r.Out -match 'f2.cat \(Sha256\)') "content change with the time kept detected: $($r.Out)"
	Set-Content -LiteralPath (Join-Path $inst 'new.dll') -Value 'n' -NoNewline
	Remove-Item -LiteralPath (Join-Path $inst 'Update\Patch\p.cat')
	Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', $m3) | Out-Null
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $m1, '-After', $m3)
	Check ($r.Code -eq 1 -and $r.Out -match 'added    new.dll' -and $r.Out -match 'removed  Update\\Patch\\p.cat') "add/delete detected: $($r.Out)"
	$bad = Join-Path $outd 'bad.csv'
	Set-Content -LiteralPath $bad -Value "`"Path`",`"Size`"`n`"a`",`"1`""
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $m1, '-After', $bad)
	Check ($r.Code -eq 1 -and $r.Out -match 'header mismatch') "header mismatch: $($r.Out)"
	$empty = Join-Path $outd 'empty.csv'
	Set-Content -LiteralPath $empty -Value '"Path","Dir","Length","LastWriteUtc","CreationUtc","Sha256"'
	$r = Invoke-Ps 'install-compare.ps1' @('-Before', $empty, '-After', $m1)
	Check ($r.Code -eq 1 -and $r.Out -match 'empty manifest') "empty manifest: $($r.Out)"

	# -Out refusals
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', (Join-Path $inst 'm.csv'))
	Check ($r.Code -eq 2 -and -not (Test-Path (Join-Path $inst 'm.csv'))) "out inside root refused: $($r.Out)"
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', (Join-Path $outd '..\install\Data\m.csv'))
	Check ($r.Code -eq 2) "out via .. refused: $($r.Out)"
	$lnk = Join-Path $Scratch 'lnk'
	New-Item -ItemType Junction -Path $lnk -Target $inst | Out-Null
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', (Join-Path $lnk 'Data\m.csv'))
	Check ($r.Code -eq 2 -and -not (Test-Path (Join-Path $inst 'Data\m.csv'))) "out via junction refused: $($r.Out)"
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', 'C:\Program Files\EA Games\Battlefield 3\m.csv')
	Check ($r.Code -eq 2) "out inside the real install refused (lexical): $($r.Out)"
	$r = Invoke-Ps 'install-manifest.ps1' @('-Root', $inst, '-Out', 'C:\Users\olku\Documents\Battlefield 3\Server\m.csv')
	Check ($r.Code -eq 2) "out inside the normal instance refused (lexical): $($r.Out)"
	(Get-Item $lnk).Delete()

	# ---------------- setup-instance ----------------
	$docs = Join-Path $Scratch 'docs\Battlefield 3'
	New-Item -ItemType Directory -Path $docs -Force | Out-Null
	$tgt = Join-Path $docs 'ServerMeasure'
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $tgt, '-Session', 's1', '-WhatIf')
	Check ($r.Code -eq 0 -and -not (Test-Path $tgt) -and $r.Out -match 'What if' -and $r.Out -match 'WhatIf: nothing written') "WhatIf writes nothing: $($r.Out.Substring(0, [Math]::Min(300, $r.Out.Length)))"
	$key = Join-Path $Scratch 'fake.key'
	$kb = New-Object byte[] 96
	[Security.Cryptography.RandomNumberGenerator]::Fill($kb)
	[IO.File]::WriteAllBytes($key, $kb)
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $tgt, '-Session', 's1', '-CopyKeyFrom', $key)
	Check ($r.Code -eq 0) "first run: $($r.Out)"
	$pwLine = ($r.Out -split "`n" | Where-Object { $_ -match '^password: [a-z0-9]{12}$' })
	Check ($null -ne $pwLine) 'password shown once'
	Check ([Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes((Join-Path $tgt 'server.key')), [byte[]]$kb)) 'key copied byte-exact'
	$b64 = [Convert]::ToBase64String($kb)
	$hex = [Convert]::ToHexString($kb)
	Check (-not ($r.Out.Contains($b64.Substring(0, 16)) -or $r.Out.Contains($hex.Substring(0, 16)))) 'key bytes never printed'
	Check ([IO.File]::ReadAllText((Join-Path $tgt 'Admin\ModList.txt')) -ceq "mharness`r`n") 'ModList exactly mharness'
	Check ([IO.File]::ReadAllText((Join-Path $tgt 'Admin\MapList.txt')) -ceq "MP_Subway ConquestSmall0 1`r`nMP_001 ConquestLarge0 1`r`n") 'MapList s1'
	$stx = [IO.File]::ReadAllText((Join-Path $tgt 'Admin\Startup.txt'))
	Check ($stx -match 'vars.serverName "mh-measure"' -and $stx -match 'vu.TimeScale 1.0' -and $stx -notmatch 'DisablePreRound') 'Startup neutral'
	Check (Test-Path (Join-Path $tgt 'Admin\Mods\mharness\ext\server\__init__.lua')) 'mod mirrored'
	$pw1 = [regex]::Match($stx, 'gamePassword "([a-z0-9]+)"').Groups[1].Value
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $tgt, '-Session', 's1')
	Check ($r.Code -eq 0 -and $r.Out -match '(?m)^no changes' -and $r.Out -match 'password: kept') "second run idempotent: $($r.Out)"
	# stale file removal stays inside the mod folder; a sibling file is kept
	Set-Content -LiteralPath (Join-Path $tgt 'Admin\Mods\mharness\stale.lua') -Value 'x'
	Set-Content -LiteralPath (Join-Path $tgt 'Admin\BanList.txt') -Value 'keep'
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $tgt, '-Session', 's1', '-DisablePreRound')
	Check ($r.Code -eq 0 -and -not (Test-Path (Join-Path $tgt 'Admin\Mods\mharness\stale.lua')) -and (Test-Path (Join-Path $tgt 'Admin\BanList.txt'))) "stale removed only inside the mod: $($r.Out)"
	$stx2 = [IO.File]::ReadAllText((Join-Path $tgt 'Admin\Startup.txt'))
	Check ($stx2 -match 'vu.DisablePreRound true' -and $stx2.Contains("gamePassword `"$pw1`"")) 'DisablePreRound keeps the password'
	Check ([Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes((Join-Path $tgt 'server.key')), [byte[]]$kb)) 'key kept without -CopyKeyFrom'

	# refusals (the normal-instance cases are lexical: the folder is never stat'ed or written)
	$docsReal = [Environment]::GetFolderPath('MyDocuments')
	foreach ($bt in @('C:\Users\olku\Documents\Battlefield 3\Server', 'C:\Users\olku\Documents\Battlefield 3\Server\sub',
			'C:\Users\olku\Documents\Battlefield 3\Server\', (Join-Path $docsReal 'Battlefield 3\Server'),
			'C:\Program Files\EA Games\Battlefield 3', 'C:\Program Files\EA Games\Battlefield 3\x',
			(Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'VeniceUnleashed\x'),
			(Join-Path $PSScriptRoot 'inside-repo'))) {
		$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $bt, '-WhatIf')
		Check ($r.Code -eq 2 -and $r.Out -match '^refused') "refused $bt : $($r.Out)"
	}
	# a junction into a stand-in of the normal instance
	$fake = Join-Path $Scratch 'fakedocs\Battlefield 3\Server'
	New-Item -ItemType Directory -Path $fake -Force | Out-Null
	$j = Join-Path $Scratch 'jn'
	New-Item -ItemType Junction -Path $j -Target $fake | Out-Null
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', (Join-Path $j 'Measure'), '-TestRefuse', $fake)
	Check ($r.Code -eq 2 -and @(Get-ChildItem $fake).Count -eq 0) "junction into the normal instance refused: $($r.Out)"
	# a target containing a junction
	$t2 = Join-Path $Scratch 't2'
	New-Item -ItemType Directory -Path $t2 -Force | Out-Null
	New-Item -ItemType Junction -Path (Join-Path $t2 'Admin') -Target $outd | Out-Null
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $t2)
	Check ($r.Code -eq 2 -and $r.Out -match 'junction') "target containing a junction refused: $($r.Out)"
	Check (-not (Test-Path (Join-Path $outd 'ModList.txt'))) 'nothing written through the junction'
	(Get-Item (Join-Path $t2 'Admin')).Delete()
	(Get-Item $j).Delete()
	# a sibling that merely starts with "Server" is accepted (WhatIf: nothing written)
	$sib = Join-Path $docsReal 'Battlefield 3\ServerMhTestDoesNotExist'
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $sib, '-WhatIf')
	Check ($r.Code -eq 0 -and -not (Test-Path $sib)) "Server* sibling accepted under WhatIf: $($r.Out.Substring(0, [Math]::Min(200, $r.Out.Length)))"
	# bad key sizes
	$k0 = Join-Path $Scratch 'zero.key'
	[IO.File]::WriteAllBytes($k0, (New-Object byte[] 0))
	$r = Invoke-Ps 'setup-instance.ps1' @('-Target', $tgt, '-CopyKeyFrom', $k0)
	Check ($r.Code -eq 2) "empty key refused: $($r.Out)"

	# ---------------- real install, read-only ----------------
	if ($WithInstall) {
		$mi = Join-Path $outd 'install-real.csv'
		$r = Invoke-Ps 'install-manifest.ps1' @('-Out', $mi)
		Check ($r.Code -eq 0) "real install manifest: $($r.Out)"
		Write-Output ("real install (read-only): {0}" -f ($r.Out -replace [regex]::Escape($mi), '<scratch>'))
	}
} catch {
	$script:fails++
	Write-Output ("FAIL exception: {0} at line {1}" -f $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber)
} finally {
	if (Test-Path $Scratch) {
		Get-ChildItem -LiteralPath $Scratch -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | ForEach-Object { $_.Delete() }
		Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue
	}
}
if ($script:fails -eq 0) { Write-Output ("powershell tests: {0} passed" -f $script:n); exit 0 }
Write-Output ("powershell tests: {0} of {1} FAILED" -f $script:fails, $script:n)
exit 1

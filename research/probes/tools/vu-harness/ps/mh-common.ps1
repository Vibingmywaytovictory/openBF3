# Shared path helpers of the S5 harness scripts (DESIGN.md 11). Dot-sourced by install-manifest.ps1,
# install-compare.ps1 and setup-instance.ps1. Nothing here writes or reads file contents.
Set-StrictMode -Version 3.0

$script:MhInstallRoot = 'C:\Program Files\EA Games\Battlefield 3'
$script:MhNormalInstanceAbs = 'C:\Users\olku\Documents\Battlefield 3\Server'

function Get-MhNormalInstances {
	# The normal VU instance, both as <MyDocuments>\Battlefield 3\Server and as the hard-coded absolute path (they
	# match today; folder redirection would split them).
	$docs = [Environment]::GetFolderPath('MyDocuments')
	$out = @($script:MhNormalInstanceAbs)
	if ($docs) { $out += (Join-Path $docs 'Battlefield 3\Server') }
	return $out | Select-Object -Unique
}

function ConvertTo-MhFull([string]$Path) {
	$full = [IO.Path]::GetFullPath($Path)
	$root = [IO.Path]::GetPathRoot($full)
	if ($full.Length -gt $root.Length) { $full = $full.TrimEnd('\', '/') }
	return $full
}

function Test-MhUnder([string]$Child, [string]$Parent) {
	# separator-aware, case-insensitive: C:\a\Server2 is not under C:\a\Server
	$c = ConvertTo-MhFull $Child
	$p = ConvertTo-MhFull $Parent
	if ([string]::Equals($c, $p, [StringComparison]::OrdinalIgnoreCase)) { return $true }
	$pp = $p
	if (-not $pp.EndsWith('\')) { $pp = $pp + '\' }
	return $c.StartsWith($pp, [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-MhRealPath([string]$Path, [string[]]$StopAt = @(), [int]$Depth = 0) {
	# Full path with every existing component resolved through LinkTarget (junctions, symlinks); the part that does
	# not exist yet is appended as is. Reads only item metadata. Once the walk is inside a -StopAt root it stops
	# stat'ing and appends the rest lexically (a link into the normal instance is never followed into it).
	if ($Depth -gt 32) { throw "link chain too deep at $Path" }
	$full = ConvertTo-MhFull $Path
	$root = [IO.Path]::GetPathRoot($full)
	$parts = @($full.Substring($root.Length).Split([char[]]@('\', '/'), [StringSplitOptions]::RemoveEmptyEntries))
	$cur = $root
	for ($i = 0; $i -lt $parts.Count; $i++) {
		foreach ($s in $StopAt) {
			if ($s -and (Test-MhUnder $cur $s)) {
				return ConvertTo-MhFull (Join-Path $cur ($parts[$i..($parts.Count - 1)] -join '\'))
			}
		}
		$next = Join-Path $cur $parts[$i]
		$item = $null
		try { $item = Get-Item -LiteralPath $next -Force -ErrorAction Stop } catch { $item = $null }
		if ($null -eq $item) {
			$rest = $parts[$i..($parts.Count - 1)] -join '\'
			return ConvertTo-MhFull (Join-Path $cur $rest)
		}
		$lt = $null
		if ($item.PSObject.Properties['LinkTarget']) { $lt = $item.LinkTarget }
		if ($lt) {
			if (-not [IO.Path]::IsPathRooted($lt)) { $lt = Join-Path $cur $lt }
			$cur = Resolve-MhRealPath $lt $StopAt ($Depth + 1)
		} else {
			$cur = ConvertTo-MhFull $item.FullName
		}
	}
	return ConvertTo-MhFull $cur
}

function Test-MhRefused([string]$Path, [string[]]$Roots) {
	# Returns the refused root that $Path is equal to or under (lexically first, then after link resolution), or $null.
	# The lexical check runs first so a refused path is never even stat'ed.
	foreach ($r in $Roots) {
		if ($r -and (Test-MhUnder $Path $r)) { return $r }
	}
	# the refused roots themselves are never resolved or stat'ed
	$real = Resolve-MhRealPath $Path $Roots
	foreach ($r in $Roots) {
		if ($r -and (Test-MhUnder $real $r)) { return $r }
	}
	return $null
}

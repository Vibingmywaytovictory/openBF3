# Compare two install manifests (DESIGN.md 11.1). Exit 0 with "install unchanged: N items"; otherwise exit 1 with the
# added / removed / changed rows (at most 50 printed) and a count line. Exit 1 also on a header mismatch, an empty or
# unreadable manifest, or a duplicate path.
#   pwsh -NoProfile -File install-compare.ps1 -Before <csv> -After <csv>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Before,
	[Parameter(Mandatory = $true)][string]$After
)
$ErrorActionPreference = 'Stop'
$HEADER = @('Path', 'Dir', 'Length', 'LastWriteUtc', 'CreationUtc', 'Sha256')

function Read-Manifest([string]$p) {
	if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "manifest not found: $p" }
	$first = Get-Content -LiteralPath $p -TotalCount 1
	if ($null -eq $first) { throw "empty manifest: $p" }
	$cols = @($first.Split(',') | ForEach-Object { $_.Trim('"') })
	if (($cols -join ',') -ne ($HEADER -join ',')) { throw "header mismatch in ${p}: $first" }
	$rows = @(Import-Csv -LiteralPath $p)
	if ($rows.Count -eq 0) { throw "empty manifest: $p" }
	$map = New-Object 'System.Collections.Generic.Dictionary[string,object]' ([StringComparer]::Ordinal)
	foreach ($r in $rows) {
		if ($map.ContainsKey($r.Path)) { throw "duplicate path in ${p}: $($r.Path)" }
		$map[$r.Path] = $r
	}
	return $map
}

try {
	$a = Read-Manifest $Before
	$b = Read-Manifest $After
} catch {
	Write-Output ("install-compare: {0}" -f $_.Exception.Message)
	exit 1
}

$diff = New-Object System.Collections.Generic.List[string]
$nAdd = 0; $nRem = 0; $nChg = 0
foreach ($k in ($a.Keys | Sort-Object -CaseSensitive)) {
	if (-not $b.ContainsKey($k)) {
		$nRem++
		$diff.Add("removed  $k")
		continue
	}
	$x = $a[$k]; $y = $b[$k]
	$fields = @()
	foreach ($f in $HEADER) { if ($x.$f -cne $y.$f) { $fields += $f } }
	if ($fields.Count -gt 0) {
		$nChg++
		$diff.Add(("changed  {0} ({1})" -f $k, ($fields -join ', ')))
	}
}
foreach ($k in ($b.Keys | Sort-Object -CaseSensitive)) {
	if (-not $a.ContainsKey($k)) {
		$nAdd++
		$diff.Add("added    $k")
	}
}
if ($diff.Count -eq 0) {
	Write-Output ("install unchanged: {0} items" -f $a.Count)
	exit 0
}
$diff | Select-Object -First 50 | ForEach-Object { Write-Output $_ }
if ($diff.Count -gt 50) { Write-Output ("... {0} more" -f ($diff.Count - 50)) }
Write-Output ("install CHANGED: {0} added, {1} removed, {2} changed" -f $nAdd, $nRem, $nChg)
exit 1

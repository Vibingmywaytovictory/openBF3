# Read-only manifest of the BF3 install (DESIGN.md 11.1, research gap-r1 section 4.2).
# Per item (the root included): Path (relative, "." for the root), Dir, Length, LastWriteUtc, CreationUtc, and Sha256
# for .toc .cat .mft .json .xml .exe .dll and initfs_Win32. LastAccessTime is never read. Refuses an -Out that
# resolves (full path, junctions, symlinks) inside the install, inside -Root, or inside the normal VU instance.
# Read errors are fatal (no silent gaps). Prints one line: "<N> items -> <out>".
#   pwsh -NoProfile -File install-manifest.ps1 -Out <csv> [-Root <install>]
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Out,
	[string]$Root = 'C:\Program Files\EA Games\Battlefield 3'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mh-common.ps1')

$HASHED = @('.toc', '.cat', '.mft', '.json', '.xml', '.exe', '.dll')

try {
	if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw "root not found: $Root" }
	$rootFull = ConvertTo-MhFull $Root
	$refuse = @($script:MhInstallRoot, $rootFull) + @(Get-MhNormalInstances)
	$hit = Test-MhRefused $Out $refuse
	if ($hit) {
		Write-Output "refused: -Out resolves inside $hit"
		exit 2
	}
	$outFull = ConvertTo-MhFull $Out
	$rootItem = Get-Item -LiteralPath $rootFull -Force
	$items = @($rootItem) + @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force -ErrorAction Stop)
	$rows = New-Object System.Collections.Generic.List[object]
	foreach ($it in $items) {
		# enumeration returns the parent index's copy of times and sizes, which NTFS updates lazily; Refresh() reads
		# the item's own record
		$it.Refresh()
		$full = ConvertTo-MhFull $it.FullName
		if ($full.Length -eq $rootFull.Length) { $rel = '.' } else { $rel = $full.Substring($rootFull.Length).TrimStart('\') }
		$isDir = $it.PSIsContainer
		$len = ''
		$sha = ''
		if (-not $isDir) {
			$len = [string]$it.Length
			$ext = [IO.Path]::GetExtension($it.Name).ToLowerInvariant()
			if ($HASHED -contains $ext -or $it.Name -ieq 'initfs_Win32') {
				$sha = (Get-FileHash -LiteralPath $it.FullName -Algorithm SHA256).Hash
			}
		}
		$rows.Add([pscustomobject][ordered]@{
				Path         = $rel
				Dir          = [string]$isDir
				Length       = $len
				LastWriteUtc = $it.LastWriteTimeUtc.ToString('o')
				CreationUtc  = $it.CreationTimeUtc.ToString('o')
				Sha256       = $sha
			})
	}
	$sorted = $rows | Sort-Object -Property @{ Expression = { $_.Path }; Ascending = $true } -CaseSensitive
	$parent = Split-Path -Parent $outFull
	if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
	$sorted | Export-Csv -LiteralPath $outFull -NoTypeInformation -Encoding utf8NoBOM
	$nh = @($rows | Where-Object { $_.Sha256 -ne '' }).Count
	Write-Output ("{0} items -> {1} ({2} hashed)" -f $rows.Count, $outFull, $nh)
	exit 0
} catch {
	Write-Output ("install-manifest failed: {0}" -f $_.Exception.Message)
	exit 1
}

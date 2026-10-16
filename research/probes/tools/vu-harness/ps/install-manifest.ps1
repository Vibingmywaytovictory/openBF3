# Read-only manifest of the BF3 install (SKELETON; DESIGN.md 11.1, research gap-r1 section 4.2).
# Per item: Path, Dir, Length, LastWriteUtc, CreationUtc, SHA-256 for .toc .cat .mft .json .xml .exe .dll and
# initfs_Win32. Refuses an -Out that resolves (full path, junctions, symlinks) inside the install or the normal VU
# instance. Prints "<N> items -> <out>".
[CmdletBinding()]
param(
	[string]$Root = 'C:\Program Files\EA Games\Battlefield 3',
	[Parameter(Mandatory = $true)][string]$Out
)
Write-Output 'install-manifest.ps1: not built yet (skeleton)'
exit 3

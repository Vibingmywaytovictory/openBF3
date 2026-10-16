# Compare two install manifests (SKELETON; DESIGN.md 11.1). Exit 0 with "install unchanged: N items", else exit 1
# with the added / removed / changed rows; exit 1 also on a header mismatch or an empty manifest.
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Before,
	[Parameter(Mandatory = $true)][string]$After
)
Write-Output 'install-compare.ps1: not built yet (skeleton)'
exit 3

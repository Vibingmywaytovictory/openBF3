# Create or refresh the measurement instance (SKELETON; DESIGN.md 11.2). THE USER RUNS THIS SCRIPT.
# Writes Admin\ModList.txt (exactly mharness), Admin\MapList.txt per session, the neutral Admin\Startup.txt, and
# mirrors mod\mharness into Admin\Mods\mharness. Refuses the normal instance (...\Battlefield 3\Server and below),
# the BF3 install, %LOCALAPPDATA%\VeniceUnleashed and the repo. -CopyKeyFrom copies the named file's bytes to
# <target>\server.key and never prints them. Idempotent; -WhatIf prints every intended write.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
	[string]$Target = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Battlefield 3\ServerMeasure'),
	[ValidateSet('p0', 's1', 's2', 's3')][string]$Session = 's1',
	[string]$Password,
	[switch]$NewPassword,
	[string]$CopyKeyFrom,
	[switch]$DisablePreRound
)
Write-Output 'setup-instance.ps1: not built yet (skeleton)'
exit 3

# old_mod.ps1 <binaries\x64> [profiles folder]
# Takes out the replay_unlock mod that earlier packs put in mods\ and turned on in every profile (the line
# "replay_unlock:0" in {mods ...} of profiles\<id>\options.set): the Replays tab comes from iphlpapi.dll now.
# Nothing to do when it is not there.
param([string]$Bin, [string]$Profiles = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\gates of hell\profiles'))
$ErrorActionPreference = 'Stop'
$dest = Join-Path $Bin '..\..\mods\replay_unlock'
$entry = '"replay_unlock:0"'
$sets = @(Get-ChildItem -Path $Profiles -Filter options.set -Recurse -Depth 1 -ErrorAction SilentlyContinue)
foreach ($s in $sets) {
    $t = [IO.File]::ReadAllText($s.FullName)
    if (-not $t.Contains($entry)) { continue }
    $t = $t -replace "[ \t]*$([regex]::Escape($entry))\r?\n", ''
    [IO.File]::WriteAllText($s.FullName, $t)
    "old replay_unlock off in $($s.FullName)"
}
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest; "old replay_unlock mod removed" }

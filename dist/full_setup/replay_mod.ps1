# replay_mod.ps1 <binaries\x64> install|uninstall
# Puts the replay_unlock mod (the Replays tab in Extras) into the game's mods folder and turns it on in every
# profile: the game activates the mods listed in {mods ...} of profiles\<id>\options.set, by folder name.
# The mod changes only interface layouts, which the lobby's resource check does not cover: vanilla-compatible.
param([string]$Bin, [string]$Action)
$ErrorActionPreference = 'Stop'
$mods = Join-Path $Bin '..\..\mods'
$dest = Join-Path $mods 'replay_unlock'
$entry = '"replay_unlock:0"'
$profiles = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\gates of hell\profiles'
$sets = @(Get-ChildItem -Path $profiles -Filter options.set -Recurse -Depth 1 -ErrorAction SilentlyContinue)

if ($Action -eq 'install') {
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -Recurse -Force (Join-Path $PSScriptRoot 'files\replay_unlock\*') $dest
    foreach ($s in $sets) {
        $t = [IO.File]::ReadAllText($s.FullName)
        if ($t.Contains($entry)) { continue }
        if (-not (Test-Path "$($s.FullName).bak")) { Copy-Item $s.FullName "$($s.FullName).bak" }
        if ($t -match '\{mods\r?\n') { $t = $t -replace '(\{mods\r?\n)', "`$1`t`t$entry`r`n" }
        else { $t = $t -replace '\}\s*$', "`t{mods`r`n`t`t$entry`r`n`t}`r`n}`r`n" }
        [IO.File]::WriteAllText($s.FullName, $t)
    }
    "mod: $dest, on in $($sets.Count) profile(s)"
} else {
    foreach ($s in $sets) {
        $t = [IO.File]::ReadAllText($s.FullName)
        if (-not $t.Contains($entry)) { continue }
        $t = $t -replace "[ \t]*$([regex]::Escape($entry))\r?\n", ''
        [IO.File]::WriteAllText($s.FullName, $t)
    }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    "mod removed"
}

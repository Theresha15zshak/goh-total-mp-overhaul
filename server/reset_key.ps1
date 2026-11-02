# reset_key.ps1 <SteamID64>
# Lets a SteamID be claimed again: the next heartbeat with a key takes it (a player on a new PC who lost
# Documents\My Games\gates of hell\goh_mod_key.txt, or a SteamID someone else claimed first).
# Run from this folder; needs `npx wrangler login` done once.
param([Parameter(Mandatory)][ValidatePattern('^7656119\d{10}$')][string]$SteamId)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
npx wrangler d1 execute goh-online --remote --command "UPDATE players SET key_hash = NULL WHERE steam_id = '$SteamId'"

GoH TOTAL MP OVERHAUL — mod for Gates of Hell 1.067
PVP rating, online list, global chat, replays, charts, competitive balance
==========================================================================

Install: close the game, unpack the archive anywhere and run install.bat.
  It finds the game in Steam and puts into binaries\x64:
    iphlpapi.dll       - the mod itself
    xvidcore.dll       - REA replay fix (the game's own xvidcore.dll is kept as xvidcore_orig.dll)
    goh_snapshots.ini  - turns on instant replay seeking
  The mod changes only the interface: you can play with players who don't have it.
  If the game is not found: put all files of the archive into ...\Call to Arms - Gates of Hell\binaries\x64
  and run install.bat from there. Or install by hand: MANUAL_INSTALL_EN.txt.

Remove: close the game and run uninstall.bat - the game is vanilla again.

Language: the mod follows the game's language in Steam (Russian or English). To set it by hand, add to
  binaries\x64\goh_online.ini:
    [ui]
    language=en

What it adds
------------
  PVP: Multiplayer -> Internet -> PVP button. Tabs: Online (who plays with the mod now), My stats
    (MMR, history, win rate by nation), Leaders. Rated: Axis vs Allies in Battle Zones and Domination,
    taken from the replays the game writes after every battle.
  MMR under names in the lobby, a Ping column in the list of games.
  Global chat: above the friends on the Internet page and on the GLOBAL CHAT tab in the lobby (RU and ENG).
    /invite in the chat - a "Join lobby" button for everyone else. Right click on a name: mention, Steam
    profile. Messages are wiped every two hours.
  Replays: Extras -> Replays, files named by mode, map, nation and date, old ones are kept.
    F8 - replay panel (speed, seeking), F9 - match charts (in a replay and after a battle).
  F7 - competitive balance (WIP: both players tick it before the start, "Balance version" must match;
    don't use it in regular multiplayer).
  F6 - the online window.

Sign in through Steam and your data
-----------------------------------
  At the first start the mod asks whether to show you online and offers to sign in through Steam. The sign-in
  happens on the official page steamcommunity.com - the mod sees no password and no session, only your SteamID.
  Without it: you are Anonymous in the chat and your matches don't count in the rating.
  The mod's server receives: your SteamID, Steam name, balance version, the results of rated matches from
  the replays and your chat messages. Nothing else.
  The mod makes a key Documents\My Games\gates of hell\goh_mod_key.txt - it ties your SteamID to you on the
  mod's server. Don't delete it and don't send it to anyone; you can copy it to another PC.

Checking the file
-----------------
  Bottom left in the menus the mod says whether your build is official. By hand:
    certutil -hashfile "...\binaries\x64\iphlpapi.dll" SHA256
  and compare with https://goh-online.goh-mod.workers.dev/versions.html

If something goes wrong (black screen, crash), send:
  binaries\x64\goh_replay.log (and goh_replay_server.log if you hosted a lobby)
  Documents\my games\gates of hell\log\game.log

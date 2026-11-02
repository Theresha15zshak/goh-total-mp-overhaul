# GoH TOTAL MP OVERHAUL

PVP rating, global chat, replays and match charts for Gates of Hell.

A client-side mod for Call to Arms - Gates of Hell 1.067, shipped as one proxy DLL (`iphlpapi.dll` in the
game's `binaries\x64`). Vanilla-compatible: the game's files on disk stay as they are and you can play with
people who don't have the mod. Russian and English (it follows the game's language in Steam).

**Download:** [Releases](../../releases) — unpack, close the game, run `install.bat` (`uninstall.bat` removes it all).

- **PVP rating (MMR):** Axis vs Allies in Battle Zones and Domination, 1v1 and teams, counted from the replays
  the game writes; a PVP button on the Internet page with who is online, your stats and the leaders; MMR under
  names in the lobby.
- **Global chat:** RU and ENG channels on the Internet page and a GLOBAL CHAT tab in the lobby; `/invite` puts
  a "Join lobby" button in the chat; right click a name to mention someone (they hear a sound).
- **Replays:** a Replays tab in Extras, files named by mode, map, nation and date; pause, speed, seeking (Shift+F8);
  match charts (Shift+F9).
- **Sign in through Steam** (Steam OpenID on steamcommunity.com): the mod gets only your SteamID, never a
  password or a session.
- **Competitive balance** (Shift+F7, work in progress).
- Bottom left in the menus the mod says whether your DLL is an official build; check by hand against
  [the list of official builds](https://goh-online.goh-mod.workers.dev/versions.html).

## Русский

Клиентский мод для Gates of Hell 1.067: PVP-рейтинг (MMR) по реплеям, кнопка PVP со списком онлайна,
статистикой и лидерами, глобальный чат RU / ENG (с `/invite` и упоминаниями),
вкладка «Реплеи» с перемоткой и графиками матча, вход через Steam (только SteamID). С игроками без мода
играть можно. Скачать: [Releases](../../releases), запустить `install.bat`.

## Layout

| Path | What |
|---|---|
| `dll/` | the DLL: `goh_replay.c` (entry, replay logic), `ui.cpp` (ImGui interface), `online.c` (server, chat), `pvp.c` (the game's pages), `steamlobby.c`, `report.c`, `balance.c`, `snap.c`, `music.cpp` |
| `tools/build.py` | builds `build/iphlpapi.dll` (`python tools/build.py`, or `competitive` for the switch-only build) |
| `tools/release.py` | puts the builds' SHA-256 on the server's list of official builds |
| `server/` | Cloudflare Worker + D1: online list, ratings, chat, Steam sign-in (`npx wrangler deploy`) |
| `re/` | reverse-engineering notes (`NOTES.md`) and tools (`gx.py`, `twin.py`) |
| `dist/` | the install pack: scripts and readmes (the DLLs are added when packing) |
| `third_party/` | Dear ImGui and stb (submodules) |

## Build

```
git clone --recursive <this repo>
pip install --target .tools/zig ziglang
python tools/build.py
```

The build reads a few files from the installed game (balance and layout copies are made from them).
A DLL you build yourself is not on the list of official builds, so the mod will say so in the menus.

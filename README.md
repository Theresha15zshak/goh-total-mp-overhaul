# GoH mod: replays, match charts, competitive balance, PVP

A client-side mod for Call to Arms - Gates of Hell 1.067, shipped as one proxy DLL (`iphlpapi.dll` in the
game's `binaries\x64`). Vanilla-compatible: nothing on disk changes, the lobby's resource check is untouched.

- **Replays:** pause, speed 1/4..16x, seek and rewind, instant jumps from in-memory snapshots, a player's own camera.
- **Match charts** (F9): points, army, kills per player, in replays and after a battle (never during one).
- **Competitive balance** (F7): a switch for a few balance files, followed by the match server a hosted game starts.
- **Online / PVP:** a PVP button in the game's Internet list, players with the mod online (F6), our server `server/`.
- **Music** (F10): own battle music in place of the maps' triggers.

## Layout

| Path | What |
|---|---|
| `dll/` | the DLL: `goh_replay.c` (entry, replay logic), `ui.cpp` (ImGui panel), `balance.c`, `online.c`, `pvp.c`, `snap.c`, `music.cpp` |
| `tools/build.py` | builds `build/iphlpapi.dll` (`python tools/build.py`, or `competitive` for the switch-only build) |
| `server/` | Cloudflare Worker + D1: online list, player keys (`npx wrangler deploy`) |
| `re/` | reverse-engineering notes (`NOTES.md`) and tools (`gx.py`, `twin.py`) |
| `dist/` | install packs: scripts and readmes (the DLLs and the replay mod's layouts are added when packing) |
| `third_party/` | Dear ImGui and stb (submodules) |

## Build

```
git clone --recursive <this repo>
pip install --target .tools/zig ziglang
python tools/build.py
```

The build reads a few files from the installed game (balance and layout copies are made from them).

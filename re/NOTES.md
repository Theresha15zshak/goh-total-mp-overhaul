# Replay rewind — reverse-engineering notes (GoH 1.067, exe built 2026-09-23)

All addresses are RVAs in `call_to_arms.exe` (image base 0x140000000).
Tooling: `re/gx.py` (string/xref/RTTI index, capstone), `re/vars.txt` (all 1162 registered console vars).

## Console variables
Static records `{char* name; type*; value}` (value at rec+0x10), linked into the global list at `15288e8`.
Type ptrs: `14b1be8` float, `14b1bd0` int, `14b0bb0` bool.

## How a replay plays
- `Multiplayer::eReplayManager` (`mp_replay.cpp`): StartReplay `52e9a0`, StartRecord `52e04b`, Stop `52ead0`.
- `eReplayGameServer` (vtbl `1038b40`) inherits almost everything from the live MP server (`c5…`–`c7…`);
  own methods are only trivial stubs (`268bb0` type 0x1b, `268cb0`, `268e40`...). So a replay runs through the normal
  lockstep pipeline: server emits quants -> client schedules and executes them.
- In-game replay UI `eReplayControlDialog` (`ba4c00`, `ba5260`, id `ui_replay_control`): only "Current Player" and
  `replay/fow_switch`. No speed, no seek.

## Pacing (where fast-forward lives)
- Quant = 50 ms of game time (`10099b0` = 50.0). ms->ticks via `65e730` (x1000), `14776c8` = 1000.
- Server `c6c0a0`: real ms between quants. If `mp_serverDQ` (int, value `148da28`) != 0 it is used directly,
  else derived from the slowest client's reported speed * `mp_serverDQfactor` (1.4), capped by `mpServerMaxDQspeed`.
  `eSpeedSelectorDialog` ("Quant Speed" slider, `186f40`) sets mp_serverDQ 0..65; in a networked session it is sent as
  command 0x69 instead (`c7b21d` applies it).
- Client `ScheduleQuants` `53a790`: interval = delta * ratio * k; when queued lag > `mp_minClientSpeedupLag` (80 ms)
  k goes linearly from `mp_minClientSpeedupFactor` 0.8 to `mp_maxClientSpeedupFactor` 0.2 at `mp_maxClientSpeedupLag` 300 ms.

Correction (tested live 2026-10-30): mp_serverDQ + client factors did nothing in a replay. Reading further:
- mp_serverDQ ends up in the quant's `delta` field = game ms per quant -> touching it risks desync. Don't.
- Quant request `{time, delta, amount, ratio, ...}`. Server tick `c6b2e0`: next emit time
  `[srv+0x28] += delta * ratio * amount`; client `ScheduleQuants` spaces quants by `delta * ratio * k`.
- `c6c0a0` floors ratio at 1.0 every quant: `c6c14c movss xmm8,[10004d0]` (1.0), stored at `c6c387`.
  => speed knob = redirect that load to our own float (ratio = 1/speed). `goh_speed.py` does this live via
  WriteProcessMemory (cell allocated just past the image, rip-relative). Patch toggles fine; effect on a live replay untested.
- Possible second clamp: `c6b48a` loads 1.0 into xmm7 and `c6b5a1` resets ratio to 1.0 when [srv+0x70] && [srv+0xa0].

## Live state used by the overlay (goh_mem.py) — verified in a running replay 2026-10-30
- MP singleton `15a4eb0`: `+0x48` eReplaySessionClient (vtbl `113e3f8`), `+0x50` eGameClient (its +0xb0 is 0 in a
  replay), `+0xe0` eReplayGameServer (vtbl `1038b40`), `+0xe8` eReplayServerService.
- Core = `[replay server + 0xb0]`, class eQuantServerJob (vtbl `113d488`); appears a few seconds after load.
- Pause: `c6b190(core, paused, broadcast)` sets `core+0x21`; on unpause `core+0x28` (next quant QPC) = now + `[1556db8]`.
  Tested: paused rate x0.07 (queued quants drain), unpause x0.99.
- Speed patch tested: 2x -> x1.64 over the first 3 s (client ramps up), 1x restores the original code.
- Game time ms: `[1556e48]`, steady ~1000/s; resets when a replay loads.
- Replay manager static `153e2b0` (vtbl `10b7138`), `+8` session, path std::string at session+8,
  profile-relative like `//replays/x.rep`. Some autoreplays have no gameEndTime in replay.ss -> no duration.
  eReplayGameServer start `c7dfc0` takes the path from `[[srv+0x98]+0x228]`.
- Autoreplay names (2026-10-31): a new recording session `52df60` (from `c7529b`) calls
  `52ebb0(mgr, out, "autoreplay", 5)`. It lists `replays/*.rep`, reads each file's number (`69a540`), uses
  the lowest free 1..N, and once all N exist overwrites the oldest file. N=5 is an immediate (`mov r9d,5` at `52dfd6`); the DLL makes it 9999.

## Measured speeds (2026-10-30, this PC)
1x->1.00, 4x->4.06, 16x/30x->7.7 (CPU ceiling). Seek uses 8x.

Why 7.7x (found 2026-10-30): the client plays ONE quant per frame. Logic-loop frame (57b750) dispatches events
0x16, 0x17, 0x1a, 0x1d (kicks the "quant" thread job, 583150 -> a5830), 0x20 (waits for it), 0x26, 0x23, 0x29.
`mp.thread_game.exec` 53aca0 takes one due quant off the client queue ([obj+0x30] list, [obj+0x38] count),
puts its delta into [154c158] (57b3b0) and its ratio into [1556e50]; the next frame steps the clock
(65f380 on timer 1556e00, game ms at +0x48) and runs "scene:quant" a1b320 on the quant thread.
Drawing is a parallel job: 581ca0 -> 694960 "render.scr" (render.gui, render.tgt, Present); the frame waits
for both. So the seek ceiling = frames/s x 50 ms. Seek now skips 694960 except 4 frames/s.
Server core (= rcx of c6b2e0, same object as replay_core()): +0x40 int start ms of the last batch sent,
+0x44 delta ms, +0x48 amount, +0x4c ratio. Seek lets the server run at ratio 1/50 and holds it (core+0x21)
once start+delta*amount >= target; the client drains its queue to there.

## Toward rewind (restart the replay in-process)
- Build tools: `python -m ziglang cc -target x86_64-windows-gnu` from `.tools/zig` (pip `ziglang`), builds a DLL in ~30 s.
- Replays list "Load" = `2637e0(eGUI_ReplaysDialog*, eReplayInfo* item)`: copies session descriptor `[item+0x7c8]`
  (`6857e0`), creates eReplayGameServer (`c7a920` base ctor, size 0x150) etc. The running replay server keeps its
  descriptor copy at `[srv+0x98]`; path at `[desc+0x228]` (std::string).
- In-game menu command dispatcher `bd4990` (ids 0x10b..0x118, table `bd5590`): 0x110 restart-confirmed
  (`a7f870(1)`, `a883c0(1)`, `a7f8b0()` - these look like pause-level/page helpers, restart itself goes through
  events), 0x111 restart popup, 0x112 exit-confirmed (`a883c0(1)` then event 0xfc2), 0x113 exit popup.
- Exit to menu logs "Exit game mode" (`a69000`); after exit the game lands on page_extras with the replay list.

## The mod as shipped (2026-10-30)
One file: `build/iphlpapi.dll` -> game `binaries\x64\iphlpapi.dll`. Only the exe imports IPHLPAPI among the game's
DLLs (version.dll / winmm.dll are taken by other mods). It forwards all 313 System32 iphlpapi exports
(`tools/gen_proxy.py` -> `dll/proxy_iphlpapi.c`, asm jmp stubs + .drectve exports), then patches the game.
UI: Dear ImGui v1.91.9 drawn via swapchain vtable hook (Present 8 / ResizeBuffers 13) taken from a dummy swapchain.
Build: `python tools/build.py` (zig 0.16; never pass -static with -shared, zig then emits an ar archive).
Log: `binaries\x64\goh_replay.log` (rewritten each start).

## Threads (from the DLL log)
- Process main thread: `697a40` -> loop `57c100` -> `145cf0` per frame (window/render, Present).
- Logic thread (GUI handlers, replay Load, server tick, exit): proc `a5aa0` -> loop `57b750` (body 57b881..57bdff);
  `57b8b5` calls `33a2b0` = GUI update once per iteration -> rewind state machine hooks its entry.
- GUI event 0xfc2 sent from the main thread did exit the replay, but only after ~14 s (through the MP session).

## Camera speed ceilings
`[14768a8]` float pairs {min,max}: scroll {0.03,0.3}, rotate {0.1,2.0}, third person {0.03,0.3}.
`38bf77` validates options against them (out of range -> reset); the DLL lifts the maxima to 3 / 10 / 3 at startup.

## Player view (own camera)
No network message carries the camera (cmCameraMoved/Updated are local GUI events), so replays have no camera.
Camera object: static `159ae88`: +0 ViewAngle, +4 YawAngle, +8 FOV, +0xc Distance, +0x20 look-at xyz.
eCameraState (vtbl `10fdbf0`, 0x28 bytes): +8 x,y, +0x10 z, +0x14 dist, +0x18 pitch, +0x1c yaw, +0x20 fov, +0x24 valid.
Get = `6dff60(state, camera)`, Set = `6e0000(state, camera, instant)` (save/load uses these; logic thread).
DLL records 20 Hz into Documents\my games\gates of hell\replay_camera\cam_<unix start>.bin during live MP games
(eGameClient vtbl `113d660` at singleton+0x50 and no replay client), matches replays by replay.ss gameStartTime ±60 s.

## Match charts (post-match «Графики матча» window)
- Stats object = rcx of spawn bookkeeping `d2e2a0(stats, &{unit, byte player})` (logs "dbg: player#N spawned X with
  cost C. budget a->b. totalAdd, totalSpent" to game.log). `[stats]`/`[stats+8]` = vector of records, 0x1b0 each, one per
  player; byte +0 = player number (the N of player#N).
- Record +0x118 = army size in limit points (+unit[+0x3c0] on purchase, drops on losses; +0x11c = the cap, 130),
  +0x158 tracks the same. Verified from a dump: a 135-point squad adds 27, a soldier lost removes 3.
- Record +0x70 = vector of economies (0x508 each, one per resource; the doctrine one has totalAdd 0):
  +0x40c budget, +0x430 totalAdd (earned), +0x434 totalSpent. `d2c940(record, unit)` picks a unit's economy.
- HandleStatsEvent(game, ev): `d13ba0` (frontline, battle zones, CTF, KotH, ...), `d24380` (competitive, guerrilla;
  calls eGame's `d0d9a0`). ev type 3 = killed: victim actor `[ev+0x10]`, its owner byte `+0xda8`; killer descriptor at
  ev+0x28 (copied by `a558a0`), killer player byte at ev+0x38 (0xff none; then the game substitutes the victim's owner).
- Results page: eGUI_PageGameResult vtables 10403c0 / 1040490 / 10404a0 / 10404b8, shown via `343930`.
  Not yet seen live (the test replay was closed before its end).

## Music
- MP music is not in the exe: each map's .mi has `music_new/*` triggers. Action music on any
  `shot injure explosion` signal in zone "global"; `ambient$` +4 every 4 s without one, back to ambient at 20
  with `{"music" {stop_all} {fade 8}}`. Editing .mi breaks resource matching with other lobbies -> DLL only.
- Mission action "music" runtime `9b8540`: start -> MediaManager play `4caa10(path*, channel, fade xmm2, loop r9b)`
  (returns channel or -1; 14783c0 = snd_mediaDisabled), stop -> `4cabf0(path*, fade xmm1)`, empty path = all.
  path object = std::string + cached wide copy at +0x20 (ctor 6741b0 inline-able, dtor 674210).
- "In a battle" = byte `[[159c760]+0x908]`: set to 1 by `a69130` ("Enter game mode as user N", right before
  GameThreadStart), cleared by `a69000` ("Exit game mode", event 0xfb3; its rcx is singleton+8). Works for live MP,
  skirmish and replays. The old test (eGameClient vtbl at MP singleton+0x50) missed a live 3v3 on 2026-10-30.
- eGUI_PageGameResult also pops up as an overlay in the middle of a match (game.log shows it every few minutes),
  so it is no end-of-match signal; page_show saw it with vtable rva 1059b98, not the RTTI ones above.
- DLL (`dll/music.cpp`): filter hook on 4caa10 returns -1 for "/music/..." while our director plays a battle,
  stops what already plays via 4cabf0 on the logic thread; own playback with Media Foundation + XAudio2 2.9 from
  binaries\x64\music\{calm,tension,battle}; settings in goh_music.ini; F10 window.

## Snapshots (instant jumps) — dll/snap.c, started 2026-10-31
- No restorable state in the engine: eSceneSerialize/eEntitySerialize/eActorSerialize are the map/save
  loader (not bit-exact: RNG, projectiles, animation phases); "SNAPSHOT BEGIN" (c762d2) is a desync text dump.
- Memory: operator new 36b6a0 -> [153f6d0] eNativeMemoryAllocator (vtbl 105e8a8: +0x20 alloc, +0x28 realloc,
  +0x30 free) -> CRT _aligned_malloc f5a960 -> malloc f59de0 -> HeapAlloc(GetProcessHeap()) (CRT heap 15aa460).
  The exe's only memory imports: HeapAlloc/HeapFree/HeapReAlloc/HeapSize, VirtualAlloc/VirtualFree
  (36b7d0 = VirtualAlloc(NULL, n, MEM_COMMIT, RW) page allocator). Thunks d375e0.. = jmp [IAT].
- Approach: in DllMain (before the exe's CRT) the exe's IAT slots for those 6 go to our allocator in one
  48 GB MEM_WRITE_WATCH reservation; DLLs keep the real heap. Bookkeeping lives in the range (restored with
  it), locks outside. Snapshot = range up to top + exe .data (rva 146e000, 0x13d930). Restore = pages from
  GetWriteWatch since the snapshot + .data. Done at main_tick with all other threads suspended, retried
  while any thread's RIP is in exe .text or inside our heap functions.
- Menu with our heap (2026-10-31): 1.24 GB live in 4.48M blocks, 1.57 GB committed, stable.
- External objects the restored memory may point at: D3D11 resources (created/released after the snapshot),
  MSS: AIL_allocate/release_sample_handle, AIL_open/close_stream, AIL_mem_alloc_lock_info/AIL_mem_free_lock,
  AIL_set_file_callbacks (MSS calls game code). exe TLS: 17.6 KB per thread (not restored).
- goh_snapshots.ini [snapshots] enabled=1 moves the heap; test=1 runs: snapshot at 20 s of a replay,
  restore at 40 s, compare the kills of both runs (log "snapshot test: ...", faults logged).
- Test 1 (3v3 replay): heap 8.5 GB live in 8.8M blocks; full copy 8.5 GB in 4.2 s; 20 s of battle dirtied
  only 28044 pages (109 MB), restore 98 ms. Then nvwgf2umx.dll faulted reading -1 on the render thread 5 ms
  later and the game hung: D3D objects released after the snapshot. -> dll/keep.c: Release (vtable slot 2)
  of all D3D11 classes replaced (classes found by creating one of each after D3D11CreateDeviceAndSwapChain);
  a last release from exe code keeps the object (our ref) until the replay ends. Same for MSS
  AIL_close_stream (paused instead) and AIL_mem_free_lock. D3D11 views hold their resource by an internal
  count, so the resource's public release also reaches 0 and is kept.
- Test 2: keeper fine (23 D3D classes), restore went through, then the quant thread crashed:
  eQuantExecutor:Execute > quantThread:onExecute:nq > ir:update (b77470) > unit talk sound (91beb9) > sound
  load (4d3bc0) > resource IO (663c30 "File not found", 38751d eIoStream/zip prefetch) > minizip
  unzGetCurrentFileInfo (41ae30) -> ZSEEK through unz64_s.z_filefunc (+0x20) whose memory held a string:
  an unzip handle opened after the snapshot, referenced from outside the range. Fix: save/restore the exe's
  static TLS block (TEB+0x58 -> [*AddressOfIndex], 17632 bytes) and the exe's TlsAlloc slots (TEB+0x1480,
  expansion TEB+0x1780) of every thread; threads newer than the snapshot are only counted.
  Minidumps: Documents\my games\gates of hell\log\minidumps (BugSplat); stack memory is in the memory list,
  the thread list's stack RVA is 0. Our log now keeps the previous run as goh_replay.prev.log.
- Tests 3/4 (2026-10-31): one run crashed again in minizip 2.4 s after the restore (smoke grenade sound),
  one went through clean. Cause: eZipFileReader_MT (vtbl 106adb0, getter 3fe0e0) keeps each thread's unzFile
  in its own TlsAlloc slot ([this+0x40]); the exe allocates 151 slots (one per pak), we saved only 64.
  Now MAX_SLOTS 1088 (TEB 64 + expansion 1024). eZipFileReader_ST (106ad48) keeps one at [this+0x38].
- Snapshot of 11.7 GB took 31.6 s (the copy pushed the PC into paging); restore 202 MB in 0.5 s.
- D3D keeping grew ~600 objects/s (56 673 in 2.5 min): per-frame objects. Now the device's Create* slots
  3..27 and the deferred context's FinishCommandList (slot 114) record each object's epoch (snapshots taken
  so far); an object dying in the epoch it was born in is released for real.
- Test now compares every stats event (type, actor owner byte, killer byte for kills) and every purchase,
  quant by quant, before and after the restore.
- Compact store (2026-10-31, snap.c "compact snapshots", lz.c): a session per replay. Snapshot 0 = base,
  all pages compressed by 4 helper threads while the game runs (not restorable, torn pages); snapshot 1
  right after it (every page written since the base began), then one every 15 s of game time: pages from
  GetWriteWatch copied with the world stopped, compressed afterwards. Per page a version list; a version
  is the page LZ-compressed or XOR with its key (an older full version) compressed; XOR > 1 KB starts a new
  key. Memory = snapshot g_cur + pages written since; restore to j writes those plus every page in the
  snapshots between g_cur and j, then .data pages and the per-thread data. After a restore the next
  snapshot also takes the pages of the snapshots after g_cur, so the chain stays linear.
  Codec: ~1 GB/s pack, 4 GB/s unpack; sparse XOR pages to ~2.5%.
  Offline: tests/store_test.c + build/store_test.dll (the store in a DLL, the exe plays the game; its
  VirtualAlloc is redirected too, so test bookkeeping must use kernel32's VirtualAlloc directly).
- Test 5 (autoreplay5, 3v3): base 8281 MB -> 5341 MB stored (poor ratio, per-page LZ) in 6.4 s; snapshots
  stop the game 56-154 ms and add 10-13 MB each; D3D keeper held 13-46 objects, let 128k newer ones go.
  Fast seek with snapshots on the way ran x6-7. Jump 70 s -> 6 s took 214 ms (30k pages), then 1.4 s later
  crashes in async_pather_manager (8be0e0) and the quant thread: the pather's loop (8be020) keeps its own
  task list in r15 and swaps it with the shared queue [mgr+0xc0] each round; the heap went back, r15 not.
  Fix: snapshots keep every game thread's registers and used stack (threads whose stack holds a return
  address into exe .text; this DLL's threads skipped by start address); a restore puts them back into
  threads parked at the same rip/rsp (waits up to 3 s for them to get there). Snapshots wait (up to 60
  frames) until game threads sit where they sat at the previous one. tests/store_test.c has such a worker
  and fails without the thread restore.
- Test 6 (2026-10-31, autoreplay3_sessionfix_spectator): jump 70 s -> 5.5 s with 15 game threads put back,
  then a jump forward to the snapshot at 65 s and a fast seek with snapshots to 4:20, no crash. Base 7.9 GB
  -> 5.1 GB stored. Fast seek still x3-6 at up to 25k frames/s: pacing-bound, not CPU-bound.
- Pacing during a seek: c6c0a0 recomputes the request ratio from the clients' speed (adaptive branch when
  [srv+0x70]): xmm8 = min(computed, floor + 0.5), and a ratio may drop only 0.02 per emission
  (c6c348: prev - [10d1048]=0.02). Constants: 10004d0 = 1.0 (patched to our cell), 1003158 = 0.5,
  10265e8 = 0.9. Both stores of the final ratio are now stubbed while seeking (forced to our cell):
  c6c387 movss [rdi+0Ch], xmm8 and c6b5db movss [r14+0Ch], xmm0 (adaptive batch branch of c6b2e0).
  (First build of these stubs crashed every replay on its first server tick: the site's jmp rel32 was
  computed from the local byte buffer instead of the site. Fixed 2026-10-31; after both sites the
  forced register is dead - xmm8 is reloaded at c6c38f, xmm0 is overwritten by the call at c6b5f8.)
- Compressed snapshot data now lives in %TEMP%\goh_snapshots_<pid>.bin (mapped 256 MB chunks,
  FILE_FLAG_DELETE_ON_CLOSE); "Подготовить" runs the replay to its end (duration - 1 s, or until the clock
  stands 4 s) with snapshots, then jumps back to where the user was.
- Snapshots cannot be kept across game runs: they hold pointers to D3D/sound/OS objects of that process.
  So every replay is prepared when it opens (ini prepare=1, default): a pass to the end with snapshots,
  game sound muted (AIL_set_digital_master_volume_level on the driver from AIL_open_digital_driver), our
  music off, centred progress window with «Пропустить». dll/prof.c samples the logic and quant threads
  during the pass and logs the top functions every 10 s ("prof logic/quant"); it shares an SRW lock with
  stop_world so a sample never holds the logic thread while that thread stops the world.
  33a2b0 (GUI update) = eUICommandHelper singleton -> 2be8d0 process UI commands: not worth skipping.
- Seek: a click jumps to the latest snapshot <= target when the target is behind or a snapshot is ahead
  (worker sets g_restore_req, main_tick restores, worker fast-forwards the rest). Green strip under the
  timeline = covered stretch. test=1: at 70 s jump back to 20 s, compare events up to 70 s.

## Rewind plan (variant A)
Seek back to T = restart replay (eReplayManager Stop + StartReplay same file) and fast-forward until quant >= T,
with rendering skipped. Needs: current quant number (the replay_unlock mod finds it as `A_QUANT`), restart hook.

## Variant B (snapshots) — open question
Does save-game work while a replay plays? Not checked yet.

## Prepare speed (2026-10-31)
- First in-game prepare (3019 s replay, big battle): x6 early, x3-5 later; 441 s done in 107 s. The quant thread
  is ~85-90% busy (prof), the logic thread waits for it (88% in ntdll<660950) and the server stays 1-3 s ahead:
  the simulation itself is the ceiling now, not pacing. A quant costs ~8-13 ms of CPU in a large battle.
- The quant (a1b33d, second half a1bb4b) is split into named steps on the engine's per-thread debug stack
  (what crash reports print): d1310(stack, name) pushes a 0x38-byte entry {+0 kind, +8 const char *name},
  5e37f0(stack) pops; the stack = TlsGetValue([147cb08]) (5e4190 -> 5e4c60), a vector {begin, end, cap}.
  prof.c reads it from the held thread and logs "prof quant steps: pct name | ..." (inclusive).
  Steps and their work calls: upvbu, trig 995700, anim a20920/5c4f50, collide 7762a0, async.quant 8bd780,
  ebf, contacts 733e40, bullets 8317d0, dmg a6da80, entity_1, particles 56ab40, fog 87f560, ecexec b90d0,
  psig, ecl, light, ecsend. Per-actor steps: scene:quant:actor{,:cmp,:fsm,:chs,:sen,:bup,:buq,:wpn} (7efd10..).
  Plan: skip the steps that only feed the picture while preparing (particles, light, ...), check determinism.
- a1b374: if [1485eb8] > 0 the quant spins on QPC until that many ms passed (off: the quant thread calls QPC
  only 2-5k times/s).
- Prepare screen: scenes 1/s while preparing (4/s in a seek); on skipped frames ui_overlay_frame presents the
  last scene (copied at the game's Present) with the panel, 30 fps, from the render job.
- Our own frames between skipped ones (Present from the render job outside the game's frame) crashed twice
  in render.tgt within 20-30 s (null vertex declaration at 51ffd0 -> 6e0ec0, and 6e410e): removed, back to
  4 game frames a second with the panel drawn on them.
- Step profile of a big 4-player battle (quant thread, inclusive): busy 84-89%, scene:quant 76-82%,
  entity_1 30-33% (human 22-24%, actor 22-23%: buq 6-13%, wpn 5-8%), ebf 14-15% (loop calling each
  entity's vtbl+0xb0), collide 10-13%, ge 6-12%, anim 6-8%, particles 1-7%, ecexec 1-2%, fog 1%, bullets <1%.
  By module: game 80-83%, ntdll 14-17% (idle between quants), our allocator 1%. Only particles and maybe
  anim are picture-only: skipping them could win ~10-15%, not multiples. Profiler now opt-in (profile=1).
- Third prepare crash (14:52, 6738f5 in eResource code, resource pointer = 8) came with our frames gone.
  All three look like reads of just-freed objects: our heap wrote its free-list link over a freed block's
  first 8 bytes (the vtable) and reused the newest free block at once, where the Windows heap leaves freed
  bytes alone for a while and reuses in no set order. Fix (2026-10-31): the link lives in the block header,
  freed small blocks queue FIFO and are reused only while >1 MB (>=64 blocks) of their class wait; freed
  large runs wait (256 runs / 64 MB) before going back. snap_alloc_test checks it. Not yet confirmed in game.
- The allocator change did not stop it: 15:51 crashed at 6e0ecd again, at 145 s (14:45: 140.8 s), same
  replay. Real cause: a frame is two render jobs (581ca0 mode 8 = scene/render.tgt, mode 2 = GUI/render.gui)
  and the 4-per-second filter let through whichever call came first after 250 ms, so drawn frames were
  often half frames. Now the filter decides per frame (g_frames, counted at frame start) and lets both
  jobs of that frame through. Quarantine cut to 64 KB per class / 32 large runs (1 MB doubled the dirty
  pages per snapshot: 60-80k instead of 25-40k).
- Confirmed: with the per-frame filter the log says "8 drawn/s" (4 frames x 2 jobs); before it said 4,
  i.e. every drawn frame had been half a frame. 15:56 run: prepared to 140 s, stopped, jumped back
  140 -> 5.7 s in 332 ms, fine.
- Snapshots every 15 s (user, after trying 10; SNAP_EVERY_MS in goh_replay.h). A click inside the prepared stretch (up to one
  step past the last snapshot) jumps to the snapshot at or before it and stops there, no fast-forward
  (the user's choice: instant, up to 15 s early); a click a few seconds ahead in the current stretch
  plays there; past the stretch: last snapshot, then fast-forward. The bar's tooltip shows "a -> b".

## Competitive balance switched in the lobby (research, 2026-10-31)
Goal (user): when everyone in a lobby runs the DLL, the host can switch to a competitive balance without
restarting the game. Content: the user's mod `mods/MP_Overhaul_Late` (source `../goh_mp_overhaul`):
whole-file replacements of `set/multiplayer/units/late/{units,inf}_{ger,rus}.set`,
`set/interaction_entity/{generic,explosive}.inc`, plus new breed (`set/breed/mp/...`), ammo
(`set/stuff/pistol/flare.*.ammo`) and localization files. Static findings, nothing tested in game yet:
- Match roster: `mp_game.cpp` "Game: Create game %s mode:" (`d06a00`) calls `d34df0(name1, name2)`, which
  reads `units` from the first existing `/set/multiplayer/units/%s.set` of {name1, name2, "common"} into
  the global `15a8e78`. game.log: the lobby (`page_session`) sits for minutes, "Game: Create game" comes
  when the host starts, seconds before eGUI_PageLoading. So the battle's roster is read after the lobby.
- Other copies: eUserInfo ctor `6f6370` (profile, from `6f6c30`) loads common.set into its
  eUnits@Multiplayer (+0x30) once; the lobby GUI `2a1264` loads it again (doctrine lists shown in the lobby).
  Load idiom: `66c280(reader, path, "units")` then `685450(object, node, 0)`.
- Reload commands left in the exe (console, registered via `68ed20(name, fn, desc)`):
  `ed_reloadInteractions` -> `694c00` -> `7629f0`; `v_reloadMarketSettings` -> `36a490` (`/set/market.set`,
  not the MP roster); `dev_l10n_reload` (`2bb40`). `694c10` calls a long chain of database loaders.
- Lobby resource check: syncer built by `cf0090` (from startup `bf0010` and `Multiplayer::Core::Create`
  `4219c0`). ResCRC masks `/entity/;/fx/;/properties/;/set/;/sound/`, MapCRC `/map/single/;/map/bonus/;/map/multi/`
  (config keys `resourceSyncer/crcFileMasks|excludeMasks`); exchanged as appCRC/exeCRC/resCRC/mapCRC
  (`c31ad8`, `c3b88b`). Console `mp_crc` prints them: check before/after switching that ResCRC stays put.
- Lobbies are Steam lobbies (`SteamMatchMaking009`, "Lobby data received <id>" in game.log): member data is
  a hidden channel for the DLLs' handshake.
Open: the VFS open to substitute files; whether breed/stuff files load by name on demand; refreshing the
lobby doctrine list; Steam lobby id and member data from the DLL; blocking the host's Start until all agree.
- First switch (2026-10-31): dll/balance.c hooks the repository open `663c30(path, mode)` (trampoline over
  its first 10 bytes; `6646f0` is a jmp to it, 52 callers). Test 1 failed silently: the SDL reader
  (`66dfd0`, mode 1) passes the path as written in the include ("late/units_ger.set", resolved later by
  `674f30` against a per-thread dir stack), and mode 1 has no disk fallback (`test r15d,0x106`), so an
  absolute path to our own file could not have worked either. Now: the game opens its file as usual; if the
  returned stream holds the whole file (eZipStream `1060678`: unpacked into its own buffer; +8 buffer,
  +0x20 offset 0, +0x28 end, +0x30 pos) and its CRC is the game's, our same-length version is copied over
  it (an eBufferStream `10c6258`, a view of pak memory, is pointed at a heap copy instead). First content:
  late_ger_defensive swaps 120mm sGrW 42 (t2 -> t3) and 150mm NbW 41 (t3 -> t2). F7 window; every open is
  logged ("balance: the game opens ...").
- Test 2 (2026-10-31): swap worked ("ours" at Create game) and the tiers changed, but the spawn menu
  (eGUI_GC_UnitSpawn `234530`) lays a doctrine's purchases out in file order, not by group: NbW 41 stayed
  in the bottom cell. tools/build.py can now also swap whole blocks ('swap', start1, start2), same length.
- MP crash 2026-11-01 (user + friend): `EXCEPTION_ACCESS_VIOLATION read 0x568` at `785fcc`, quant thread,
  when NbW 41 spawned (it appeared far off in a corner of the map). `785fc0(this, idx16)` = `this->[+0x58]` (the
  vehicle being boarded) `->[+0x568]` = its seat vector (0x298 bytes each), callers include `eBoarder::StartIn`
  (`78c7bc`): the crew was told to board a gun this machine did not have, `this+0x58` null. Cause: two
  versions in one match. The user still had the 08:33 build (tiers only, vanilla order: log shows sGrW #4532,
  NbW #4535 and no "pairs moved"); the friend had the build that also swaps their places. A purchase's number
  then meant NbW on one machine and the mortar on the other. Since then every build shows "Версия баланса"
  (CRC of dll/balance.c + our copies, tools/build.py `balance_tag`) in the F7 window and the log; a match
  needs the same tag and the switch on at every player until the lobby handshake exists.
- MP again 2026-11-01 12:00, same switch code on both sides, still broken: the hosted match runs in a
  server process the host's game starts, `call_to_arms_server.exe` (same engine built apart, imports
  IPHLPAPI.DLL too, its log is `Documents/My Games/gates of hell/log_ds/game.log`). It reads the files
  itself and runs the match: "dbg: player#1 spawned doctrine_120mm_sgrw42_late with cost 30" for the NbW
  the players bought, then "Attempt add order eLeave / eWeaponAiming to linked or inactive entity
  120mm_sgrw42". Our DLL stayed a plain proxy there (and its startup truncated the game's goh_replay.log:
  the NUL run at the start of that log). This was the cause of every MP failure so far (mortar in the kill
  feed, crew not boarding, desyncs, the eBoarder crash). Fix: the DLL in the server installs the same hook
  and follows the game's switch through named memory `Local\goh_balance_switch` (magic, tag, on, pid),
  logging to `goh_replay_server.log`. Server addresses (re/twin.py, each function matched whole, struct
  displacements included): repo open `47a170`, eZipStream `d6d208`, eBufferStream `dc8c98` (ctor `48fc20`),
  eUnits@Multiplayer `dd2160`, global roster `10f9538` (loader `a7a190`), users `11b8730`; number
  assignment `a77a40` uses the same +0x3e0/+0x3e8/+0x50c. re/gx.py takes GX_EXE/GX_CACHE for the server.

## Black screen without ReShade (2026-10-31)
Friends' games (and this PC once ReShade's dxgi.dll was moved out) hung or died ~2 s after start, before the
menu. Bisected with [debug] switches in goh_snapshots.ini (balance, watch, main_iter, media, filter, music,
qpc, ui, ui_draw; 0 = off): only the panel's Present hook mattered. Three faults, one after another:
- the old way made its own D3D11 device + swapchain on a side thread to read IDXGISwapChain::Present from
  the vtable while the game made its own: the game hung at start;
- hooking ResizeBuffers (vtable slot 13) hung it too (the game's loader thread stuck in SendMessage 0x419);
- hooking Present (slot 8) with the Steam overlay on: gameoverlayrenderer64 keeps "the original" Present in a
  global (+0x180370 area, call at +9b81d) that ended up pointing at our hook while it also patches DXGI, so
  the two called each other until the stack ran out (AV in ntdll __chkstk; WER, no BugSplat dump).
With ReShade our hook sat on ReShade's wrapper vtable, which is why it worked here for weeks.
Fix: nothing in DXGI is touched. The game's own present, eRenderDevice `4fc010(this)` (swapchain at
this+0xa8, `call [rax+40h]` at 4fc038; reached from the window procedure), is a WATCH entry; at its entry
ui_present draws our frame into the backbuffer, then the game presents through Steam/ReShade as usual.
The backbuffer view is made and released every frame (no ResizeBuffers hook needed). ImGui's shaders and
font atlas are made on a thread of ours with an 8 MB stack. Verified 2026-10-31: menu reached and the panel
drawn without ReShade, with snapshots off and on; the user confirmed F7.

## Online list and PVP button (2026-11-01/02) — dll/online.c, dll/pvp.c, server/
Online: the player's SteamID and Steam name come from the game's steam_api64.dll flat API
(`SteamAPI_SteamUser_v021` + `SteamAPI_ISteamUser_GetSteamID`, `SteamAPI_SteamFriends_v017` +
`SteamAPI_ISteamFriends_GetPersonaName`; null until the game has started Steam). Heartbeat every 2 min to
goh-online.goh-mod.workers.dev with a key from Documents\My Games\gates of hell\goh_mod_key.txt: the server
keeps only its SHA-256, the first key claims the SteamID (server/reset_key.ps1 frees it again).

The Internet / LAN game list is `eGUI_PageSessionList` (eGUI_PageSessionList.cpp), layout
/interface/layout/PageMultiplayerCustom.xml (interface.pak, ctor 2dfb10). Subclasses `_Online` (vtables
1049f30 main, 104a010 +0x10, 104a020 +0x28, 104a038 +0x30) and `_Lan` (104a050, 104a130, 104a140, 104a158);
destructors 2e72d0 / 2e7350 at slot 0.
- bind (main vtable slot 20, 2e0260, shared by every subclass, also Skirmish/Training): the layout is at
  page+0x58; `layout->vt[0x1e0](layout, const std::string &id)` finds a dialog by id (eDialog vt 1e0 =
  a9f760 -> 33ba80, whole tree); 171800(page, &field, id) is that plus a ref kept in the page (+0x150 header,
  +0x160 buttons, +0x170 container). A button: `__RTDynamicCast` (f1c498) eDialog (TD 14b1c58) -> eButton
  (TD 14d4fc0), then `aa4240(button, *(page+0x60), 7 /*click*/, command)` (idempotent: same entry skipped).
- commands arrive at the +0x28 sub-object's slot 1, 2e0eb0(self = page+0x28, msg), id at msg+8:
  0x15 host, 0x16 refresh, 0x18 join ... up to 0x21 (jump table); anything else goes to 340720. Ours 0x4f50.
- eDialog: +0x48 flags (bit 1 shown, bit 0 laid out), +0x98 x,y in the parent, +0xa0 w,h, +0xf0 parent,
  +0xd8/+0xe0 children; vt +0x108 (aa4ba0) = shown and the parent shown, false at a root unless the root
  overrides it (the screen's does); vt +0x60 (aaa8f0) marks the layout dirty up the chain; mouse hit test
  aa3b00 with a9ec90 (parent-relative position through each parent's vt +0xe0). Game units = screen
  pixels at 1920x1080 (root 1920x1080); the list container is at 36,266, 1097x774 there.
- our layout (tools/build.py ui_header): the game's file with a btn_pvp after JOIN, the commented-out debug
  block taken out to keep the length, swapped in the opened stream like the balance files.
- the game's fonts are AngelCode BMFont binaries (font/*.fnt) with 32-bit RLE TGA pages, stored unpacked in
  interface.pak; glyphs packed per channel (chnl 1 blue, 2 green). The list uses skin main_menu_dark_skin
  (set/registry/main_menu_dark_skin.inc in gamelogic.pak): anka ("Anka/Coder Condensed") size 15.

## Rated matches: the result comes from the game's own replay (2026-11-02) — dll/report.c
No memory digging needed: after every multiplayer match the game writes
profiles\<SteamID - 76561197960265728>\replays\autoreplayN.rep, a zip with replay.info, replay.qm (the
commands) and replay.ss, the session card in the game's text format (UTF-8):
- {main ...}: {gameType battle_zones|ammunition|...} (ammunition = «Превосходство»/Domination, key
  mp/game/ammunition in localizations/ru.pak dlg_mp_domination.po), {armySelectionMode alliance} (= «Ось против
  Союзников», mp/lobby/army_mode/alliance), {bots 0}, {map "multi/3v3_trenches:ammunition"},
  {gameStartTime unix} {gameEndTime unix};
- {members {...}{...}}: {type player} {hostId N} {name "..."} {team a|b} {army ger|fin|eng|usa|rus}
  {doctrine ...} {steamId "7656..."};
- {teams ...}: {name a} {alliance allies|axis}; {alliances ...} lists the armies of each;
- {battleInfoLast ...} / {battleInfoTotal ...}: {"<hostId>"|"a"|"b"|"game"|"info" "B64:<zlib>"} binary blocks;
  battleInfoTotal "info" = "\1" + the winning team's letter (checked against the win flag, the third int
  after the team name, in the "a"/"b" blocks, on three replays). A match left early has no winner there.
The memory census (census.c) found the same card as text in memory ({steamId "..."} {steamInfo ...}) and the
friend list (eSteamFriend) holding most SteamIDs; eSMInfo = the session list's entries.
- Why old replays vanish (2026-11-02): nothing deletes them; 52ebb0 overwrites the oldest autoreplay once
  every name 1..N is taken, N = 5 in the game. The full DLL makes N 9999; the competitive-only build did not
  (now it does), and a game without the DLL (or a friend's) is back at 5. 530210 only tells whether a name is
  an autoreplay; a replay renamed to something else is never chosen for overwriting.

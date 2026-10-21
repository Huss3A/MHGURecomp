# MHGU Recomp: status and working notes

Static recompilation of *Monster Hunter Generations Ultimate* (Switch, v1.4.0, build id `FB08F1D2...`) to a native
Windows executable, modeled on LAS Recomp (`../LASRecomp`). Goal: easy modding of game data (weapon / monster
values etc.). No game files or generated code are ever committed (see `.gitignore`).

## Key facts

- The game is **32-bit ARM** (AArch32, ARM mode only, `NX-NXFP2-a32`, NintendoSDK 4.4.2), not AArch64 like LAS.
  Modules: `main` (~4.9M reachable instructions), `sdk` (nnSdk, fully symbolized), `subsdk0` (Android Stagefright
  movie player: **not recompiled**; `movie::` API to be implemented on the host with FFmpeg, as LAS does).
- Guest memory: a 4 GB window at `GUEST_BASE` (0x10000000000); guest address `a` = host `GUEST_BASE + a`
  (`G2H` / `H2G` in `runtime/cpu.h`; the name `GPTR` clashes with windows.h). Pages are committed on first touch.
- Layout (`runtime/rt.h`): main @0x200000, sdk @0x2400000, HLE fake functions @0x3000000, TLS @0x3100000,
  HLE heap @0x4000000, heap @0x8000000 (SetHeapSize gets 2.5 GB: GetInfo total memory is reported so),
  alias @0xA8000000, stacks @0xD0000000.
- 32-bit SVC ABI derived from the SDK's svc stubs (see `runtime/kernel.c` comments): 64-bit values in register
  pairs, 5th argument in r4.
- Target hardware of the owner: i7-4770K, 8 GB RAM, AMD R9 200 series (GCN, driver 2020, **Vulkan 1.2.133**,
  1.8 GB VRAM). `build/vkprobe.exe` (tools/vkprobe.c) output, 2026-10-20:
  - missing: dynamic_rendering, synchronization2, extended_dynamic_state 1/2/3, push_descriptor,
    custom_border_color, maintenance4, index_type_uint8, robustness2, provoking_vertex, ASTC, ETC2, shaderFloat16;
  - present: imageless_framebuffer, create_renderpass2, separate_depth_stencil_layouts, timeline semaphores,
    shader_demote_to_helper_invocation, scalarBlockLayout, shaderInt8 + 8/16-bit storage, samplerMirrorClampToEdge,
    vertex_attribute_divisor, external_memory_host, line_rasterization, depth_clip_enable, BC textures,
    descriptor_indexing, inline_uniform_block, shaderInt64, robustBufferAccess, dualSrcBlend, logicOp.
  The renderer must be Vulkan 1.2 baseline (works on GTX 1080 too). LAS's Vulkan backend is 1.3: reuse its pieces,
  not wholesale.

## Recompiler (tools/)

- `nso.py` ELF32/REL loader; `lifter.py` A32 + VFP; `neon.py` Advanced SIMD (+ AES, CRC32 via helpers);
  `gen.py` function discovery (.ARM.exidx, symbols, relocs, call targets, fixpoint), recursive descent (literal pools
  never lifted; pc-relative literal loads folded to constants), jump tables (`add rB, pc, #imm / ldr rT, [rI, rB] /
  add pc, rT, rB`, with scheduled instructions in between), computed gotos ("all labels" switch fallback).
- `test_lift.py` + `test_harness.c`: differential test against Unicorn (UC_CPU_ARM_MAX). 0 mismatches over tens of
  thousands of sampled instructions (incl. all reachable VFP / NEON). Run after every lifter change:
  `make test-lift`.
- `hle_list.py` writes `runtime/hle/hle_symbols.txt` from `HLE_SDK(...)` uses (SDK functions replaced on the host:
  division helpers, memcpy & co).
- `mtarc.py`: MT Framework ARC v17 list / extract / pack / index.

## Runtime (runtime/)

Ported from LAS, adapted to 32-bit: `kernel.c` (SVCs), `ipc.c`, `loader.c` (REL relocations), `dispatch.c`,
`romfs.c` (**virtual RomFS** built in memory from the loose `romfs/` folder; mod folders `mods/<name>/romfs/`
layer on top: `mods.c`), services (`fs`, `am`, `misc` (acc, set, pctl, aoc, hid service), `time`, `audio` (LAS's
audren mixer; REV4 output layout fixed)), HLE: `vi.c`, `nvn_boot.c`, `ngc.c` (no profanity filter), `input.c`
(nn::hid at API level), `offline.c` (bcat / friends / applets as an offline console), `libc.c`.
NVN layer: `nvn/nvn.h` (argument adapter `nvn_args.c`: signature strings, 'l' = 64-bit, 'f' = float),
`objects.c`, `cmdbuf.c` ported from LAS. Renderer: `nvn/vulkan/` (Vulkan 1.2, see `vk.h`): render pass objects +
framebuffers cached by view, every image in GENERAL layout with full barriers between passes, all fixed-function
state in the pipeline key, descriptor sets from per-submission pools, guest vertex / index / uniform ranges copied
per submission (the 2.5 GB heap is not imported), textures change-detected with page write tracking
(MEM_WRITE_WATCH). `NULL_GPU=1 make` builds the null renderer instead. Shader global memory (LDG/STG) reads zeros
(logged if a shader uses it). Window and controls: `ui.c`, `host_input.c`.

## Progress (2026-10-20)

Plays: health & safety -> opening movie (FFmpeg) -> title -> character creation (name answered with MHGU_NAME) ->
style / arts -> save slot (save written to run/save/) -> Bherna village intro cutscene -> village with player
control, ~30 fps on the R9 (the game's own rate). Everything seen so far renders correctly.

Fixed on the way (2026-10-20):
- jump tables: the longer of bound-based and heuristic lengths; backward entries; scheduled instructions (VFP
  `vmov`, up to ~20 constant loads) between `add rB, pc` and `add pc`. `generated/*/undetected_tables.txt` lists
  `add pc, rX, rY` jumps not understood as tables (currently none).
- `movie::` HLE (`runtime/hle/movie.c`): deep decoded-frame queues (video held until its time on the audio clock),
  the output-format-changed event before the first frame, and GetOutputBuffer copies into the game's own Buffer.
- InlineKeyboard: DecidedEnterArgUtf8 = text inline at +0, length at +0x7d4; Calc returns the state (0 none,
  1 ready / hidden, 3 shown) - the game waits for 1 after a text entry.
- NVN fixed-function alpha test (ColorState alpha func + SetAlphaRef) in the translator (foliage, impostors).
- Vulkan loader: System32\vulkan-1.dll of older drivers has no 1.3 entry points (VMA_VULKAN_VERSION 1.2).
- Audio: MHGU sends every voice through a splitter (now implemented, plus the BufferMixer effect) - silence before.
- Movies: tight NV12 (the game copies Y then UV right after it) into the game's own Buffer memory.
- Performance: device-local vertex cache refreshed per written page (page write tracking): guest copies went from
  ~880 MB/s to ~8 MB/s; mailbox presentation (FIFO with 2 images held the game at ~26 fps).
- Settings: build\mhgurecomp.ini (runtime/config.c; written with the defaults when missing): Mode = Fullscreen /
  Windowed / Borderless, Resolution = WxH or Desktop, RenderResolution = Auto or WxH, FPS = 30 / 60. Alt+Enter
  toggles windowed / full screen. The 3D resolution is the game's own docked render size (main 0xdbcd34 / 0xdbcd38,
  `mov` 1920 / 1080, the constants the 4K-2K mod patches): tools/gen.py PATCHES makes those two instructions read
  g_render_size. Overrides for test runs: MHGU_CONFIG=<file>, MHGU_WINDOW=WxH (windowed), MHGU_RENDER=WxH,
  MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1, MHGU_PAD_POSITIONAL=0|1. [Controls] FaceButtons = Position (default: the Switch
  layout, right button A) or Label (the letter on the button, via SDL_GetGamepadButtonLabel).
- MSAA (settings MSAA = Off / 2x / 4x / 8x, F7 or MHGU_CONTROL "msaa <n>" switches it live; exec.c "MSAA"): passes
  whose depth target has the 3D resolution render into multisampled companions (TexBackend.ms) while those hold the
  target's contents or are cleared at the pass start (a look-ahead over the command list: leading_clears); a target
  is resolved only when something reads it (lazy: vk_ms_resolve). Colour: vkCmdResolveImage; depth / stencil: a copy
  into a sampleable multisampled image + a shader writing sample 0 (stencil via VK_EXT_shader_stencil_export). NVN
  alpha tests (> >= < <=) become sample coverage (gl_SampleMask, ShaderKey.alpha_coverage). Cut-outs the game does
  with its own discard stay hard-edged. R9 270X, village, 1080p: 56 fps off, 35 at 2x, 28 at 4x, 18 at 8x. Measured
  with MHGU_CONTROL "gpuprof" (GPU timestamps per pass): the multisampled passes cost ~3 ms more at 4x, a colour
  resolve ~1.6 ms, the depth / stencil resolve ~9 ms (the driver decompressing it; Vulkan 1.2 render pass resolves
  were as slow, and a sampleable multisampled image loses its compression: passes several times slower).
- FXAA (settings FXAA = On / Off): the game's FXAA pass is recognised by its fragment shader's code hash
  (FXAA_FS_HASH, exec.c) and becomes a copy of its input when off.
- Frame rate: the settings' FPS or MHGU_FRAMERATE (default 60, 30..60) writes the engine's target fps (float at +0x3c of the object in
  global 0x2326764, the field theboy181's 60fps patch sets) each frame. Targets above 60 still run at 60 (another
  cap in the engine, not investigated). MHGU_VSYNC=0/1: no vsync / classic vsync (default mailbox).
- Open: a white flashing bar the user saw in pause menu > Status (not found yet: need a screenshot); lens flare
  blocks when first entering an area (draws skipped while shaders compile: much reduced, see "Fidelity audit").

## Fixed (2026-10-20): shimmer / light-shaft "triangle" / blocky bloom (pending the user's in-game check)

Symptom: flickering 16x8 blocks in bright areas, a wedge from the sun in hunts (light shafts), sparkle on sunlit stone.
Cause: AMD's 2020 Windows driver for GCN1 (R9 200) does not flush the colour-attachment cache for a barrier that only
says ALL_COMMANDS / MEMORY_WRITE (what vk_full_barrier and the render-pass dependencies used). The next pass sampling
the image just rendered read stale 8x8 tiles, mostly of the last-drawn triangle of a full-screen quad; lockstep dumps
hid it. Fix: barriers and dependencies name every stage and access type (vk.h VK_STAGES_ALL / VK_ACCESS_ALL*).
How it was found: F10 dumps in the hunt (light-shaft chain: pass 023 extract 960x540 -> 240x135, 024-026 radial blur
fs 7a142300 with sun position in cb4[0].xy and length cb4[2].z, offsets / weights in cb5, 027 upscale, 029 composite);
a CPU emulation of the blur matched the GPU except in those tiles; a standalone Vulkan replay (tools/vkreplay.c:
shaders from run/build/glsl, dumped input, the copy + blur ping-pong) reproduced the tiles on this GPU and
showed explicit barrier flags remove them. Ruled out on the way: clears (MHGU_CLEAR_DRAW / F9), depth / stencil,
alpha test / alpha-to-coverage, primitive restart, vertex buffer size, mip data, samplers, the 60 fps patch.
- The user's Game Bar videos are much brighter than the real image (capture encoding); on-screen pixels == "shot".
- Debug aids: F10 / "dump [n]" (all passes of n frames, depth-only passes as grey + raw float32), run-debug.bat,
  MHGU_DUMP_VERBOSE=1 (targets, pipeline state, sampler, 16 floats of the first fragment uniform buffers per pass),
  MHGU_CB_WATCH=<fs addr>, MHGU_SAMPLER_LOG=1, MHGU_TEX_LOD=n, MHGU_CLEAR_DRAW=1 / F9 (clears as draws).

## Fixed (2026-10-20): holes in dynamic shadows (pending the user's in-game check)

Hard shadows had straight-edged, camera-dependent gaps next to the sharp near cascade (a dinosaur in a hunt; villagers
in Bherna). Cause: the translator ignored condition codes. The receivers' shadow lookup (e.g. fs 7abfef00, the
ground, in the screen-space shadow-mask pass) picks a cascade with `FSET.CC RZ, |x - 0.5| < c[1][1]` then
`@P1 BRA NEU`: branch when cascade 0's y range (P1) AND its x range (CC) hold. Without the CC test, pixels inside
cascade 0's y range but outside its x range jumped with an unset depth reference -> no shadow. Now FSET .CC sets the
zero / sign flags, and BRA / EXIT / KIL / SYNC / BRK / CONT test them (translate.c cc_test; 26 village shaders use it).
Also fixed on the way: nvnCommandBufferSetDepthClamp was a no-op; the game enables depth clamp for the shadow-map pass
(now PipeKey.depth_clamp -> depthClampEnable).
Debug aids added: MHGU_GLSL_OVERRIDE=1 (run/glsl_override/<addr>_fs.glsl replaces a translated fragment shader),
MHGU_FORCE_WMASK=<fs addr> (all channels written), MHGU_DUMP_DRAWS=1 (every colour draw's program in a dump),
MHGU_SAVE_MSL=1 (instruction words as comments in saved GLSL), F8 (shadow-pass scissor: as set / none / mirrored).

## Local Play over IP (2026-10-20)

The game's Local Play (Switch local wireless: Multiplayer -> Play with People Nearby) works between PCs. The game
uses nn::ldn for the wireless network and plain nn::socket for its own traffic (one UDP socket, port 12345, with
broadcasts); MT Framework's nNetwork session layer sits on top (NEX is linked too, for online play only). Both
APIs are HLE'd at the function level, so no Switch service (ldn:u, bsd:u) is involved:
- runtime/net/ldn.c: nn::ldn. Create Hub -> the host listens on ini [Multiplayer] Port (default 39500, TCP) and
  answers LAN search probes on the same UDP port. Search Hubs -> Scan asks the LAN (broadcast + this computer) and
  every ini Join address (host[:port], comma separated) for the host's NetworkInfo. Joining keeps one TCP connection
  to the host: it assigns node numbers / addresses (169.254.x.n, the host n = 1), pushes NetworkInfo on every change
  (joins, leaves, advertise data) with the state change event signalled, and relays packets between players. The
  game's packets themselves go by UDP to/from the same port when that gets through ("MHGD" datagrams with a per-
  station token; station hello every 2 s keeps NAT mappings open), else - and after 15 s without UDP - through the
  TCP link. Only the host needs a reachable port (forwarded TCP + UDP; TCP alone works). Host gone -> the stations go to state Error (reason SignalLost / DestroyedByUser):
  the game says "Connection lost." and returns to offline.
- runtime/net/vsock.c: nn::socket on that virtual network (UDP datagrams, TCP emulated with SYN/SYNACK/data/FIN/RST
  messages - the link is reliable and ordered). The Switch's values: BSD socket options, Linux errno numbers,
  O_NONBLOCK 0x800. Nothing reaches the host's real network.
- Tested on one PC with two instances (run/ hosts, run2/ joins, 640x360, MHGU_LDN_PORT / MHGU_LDN_JOIN env
  overrides): hub created, found by LAN search and by Join address, joined, both see each other walk in the hub,
  Disconnect and a killed host handled. Not tested yet: two PCs, quests together, more than 2 players.
- The game's network library is Nintendo's Pia (statically linked; RTTI names nn::pia::local / inet / session /
  transport). Local and Online play share Pia's transport (stations, ReliableSlidingWindow, RttProtocol, keep-alive /
  life check); only the session layer differs (pia::local on LDN vs pia::inet on NEX). So Local Play already adapts
  to the measured round trip like Online does - there is nothing low-ping-only to swap out. What did matter: Pia
  expects lossy datagrams, and carrying them over TCP turns a lost packet into a stall of everything behind it,
  hence the UDP path. Tested with MHGU_LDN_LAG=<ms> (received packets held back; on both ends: round trip +2x) and
  MHGU_LDN_LOSS=<percent> (received packets dropped): 300 ms, and 800 ms with 5 % loss: join, both see each other
  move, still connected after a minute idle.
- Online Play entries: nn::nifm is HLE'd as "no internet connection" (runtime/hle/offline.c); Create Hub / Search
  Hubs / Search by ID go back to the menu (the system's error popup is not shown), Friend List is empty. Before,
  nifm's SDK code called a null function (crash).
- MHGU_LOG_NET=1 logs socket setup, =2 every nn::ldn / nn::socket call.
- Kernel fix found on the way: svcWaitProcessWideKeyAtomic re-took the mutex on timeout; Horizon returns without it
  and nn::os takes it itself, so every timed-out condition variable wait (TimedWaitEvent...) left the thread waiting
  on a mutex it owned - forever. Single player rarely times out (a thread or two hung unnoticed); the network
  threads do it constantly. MHGU_DUMP_THREADS dumps now show mutex owners and a heuristic list of callers.

## Event camera pans skippable with A (2026-10-21)

Entering a village / hub area when something is new plays an event (rScheduler, arc/ev/*.arc, path
event\village\event_vNNN...) that pans the camera past the NPCs; the original can't skip it. runtime/hle/evskip.c,
hooked at uScheduler's update (gen.py PATCHES 0xb66164): A sets a playing event scheduler's speed to 30 (uScheduler:
+0x34 frame, +0x3c speed, +0x40 paused, +0x41 loop, +0x54 rScheduler, path at +0x08), so it plays out in a moment
and what it does at the end still happens (log: "[evskip] skipping <path>"). Looping stage schedulers and paused
ones are left alone. Tested with a new character's arrival in Bherna (event_v002): A during the pan -> the chief's
greeting about a second later, the dialogue then advances normally. Other events (new facility / NPC pans, story
scenes with dialogue in the middle) not tested.

## Small monsters synced in multiplayer (2026-10-21)

The original game syncs small monsters' HP / deaths only; each player's game runs their AI, so players see them in
different places. runtime/net/emsync.c makes the first player in a zone their master (the game's own large-monster
mechanism: flag 8 sends netEmAction, the others replay it); ini [Multiplayer] SyncSmallMonsters (default On).
Tested with two instances: positions agree within centimetres, hand-over when the master leaves works. Combat,
3-4 players and two PCs not tested yet. Research, data layout and test details: docs/research/small-monster-sync.md.

## Fidelity audit (2026-10-20)

Silent-bug sweep after the shimmer and shadow fixes (the two classes found: a driver needing explicit barriers, and a
translator ignoring condition codes). Diagnostics now built in: MHGU_NVN_NOOP_LOG=1 logs the first call of every NVN
command the port ignores; the translator warns when a branch tests zero / sign flags last written by a .CC
instruction whose flags it doesn't model ("[shader] ... condition-code test"); MHGU_VK_STATS=1 counts skipped draws.
Village run + the user's hunt logs: the game calls only nvnCommandBufferAddControlMemory and SetShaderScratchMemory
(both harmless); no CC warnings, translation failures or unsupported formats. Audio effects are now complete for
MHGU: I3DL2 reverb (type 5; mix 1 chains three mono I3DL2 reverbs with buffer mixers, the environmental reverb) is a
port of Ryujinx's Reverb3d, with yuzu's max + 1 delay-line ring (Ryujinx's re-reads a stale sample every 400 ms); Aux
(type 2, 6 channels on the final mix) sends each frame into the game's guest ring and plays its return, as Ryujinx's
AuxiliaryBuffer: the game keeps up in lockstep (MHGU_AUDIO_LOG=1 logs the ring offsets; MHGU_NO_AUX=1 bypasses it).
Splitter updates skipped the 12 unused bytes after each splitter's destination ids, so everything after the first
splitter in an update was dropped: stereo sounds made of two mono voices (menu confirm, load fanfare) played left only.
docs/SWITCH_COMPARE.md lists what to check against the Switch version by ear.
Draws skipped while their shaders / pipeline build (the main remaining cause of transient glitches: a flashing status
bar, missing lens-flare pieces on entering an area). MHGU_VK_STATS=1 now also logs a "2s builds" line (skips by
cause and in how many frames, the waits, build times per stage, request -> ready latency, the slowest frame). Entering
the village needed 112 new programs and ~140 pipelines at once: translation (one thread, the translator isn't
re-entrant, ~2.3 ms each) + GLSL -> SPIR-V / modules (~3.6 ms, three workers) dominate, pipelines take ~0.2 ms (Vulkan
pipeline cache), so ~1100-1230 draws were skipped over 13 frames. Now (exec.c):
- a draw whose program / pipeline is still building waits for it, up to MHGU_PIPE_WAIT_MS (default 6) per frame in
  all, and its job moves to the front of its queue;
- cache/programs.bin keeps every translated program (GLSL + reflection, keyed by a hash of the guest code the
  translator reads + the shader keys; tied to the executable's size / time stamp, so a rebuild starts it afresh), and
  cache/pipelines.bin each pipeline's state (program by content key, render pass by formats). The next run builds them
  all on the workers at its first draw (~130 programs + ~170 pipelines: well under a second, during the logos), like
  Ryujinx's shader cache. Both are appended as they grow, so a killed run keeps them. MHGU_NO_PROGRAM_CACHE=1: off.
Village load on the R9 (warm caches): 0 skipped draws on entering (was 1064 / 1230), 6 in a whole 60 s run (the first
logo frames), slowest frame 56-62 ms (was 83, the load hitch itself), steady 57 fps unchanged. First run ever (empty
caches): 761 skipped over 9 frames (was 1230 over 13): translation throughput bound, waits cap at 6 ms a frame.
Known gaps that remain:
- A program never seen before (first run, or after a rebuild) can still be missing for a few frames when many arrive
  at once.
- R9 200 approximations: no custom border colours (nearest standard colour), D24S8 stored as D32F_S8 (polygon-offset
  units differ for decals).

## Next steps
1. Modding tools: decode resident.arc tables (weapon00..14 Base/Level data: record layouts derivable from the game
   code that reads them), monster files per `arc/enemy/emXXX_YY.arc` (`attackdata`, `bodydata`, `dtbase`,
   `dttune`), JSON <-> binary converters, and per-member ARC overrides in the virtual RomFS
   (`mods/<name>/romfs/<arc path>.arc.d/<member>`).
2. Play further (hunts work: Bherna Gal -> quest -> gate left of the balloon) and fix what breaks; shader global
   memory if some effect needs it.
3. A real text entry for the name (SDL text input + an overlay) instead of MHGU_NAME.
4. A README with build instructions; Linux platform layer (runtime/platform/linux.c). It must also give
   plat_user_dir a home for save data: $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (see "Save data").
## Build / run

MSYS2 UCRT64 on PATH (`C:\msys64\ucrt64\bin;C:\msys64\usr\bin`):

    make gen GAME=../mhgu/game     # recompile (needs tools/venv: capstone, unicorn, lz4)
    make -j4                       # ~8 min full, incremental (changed chunks only) is fast
    build/mhgurecomp.exe           # or double-click it; run-game.bat / run-debug.bat start it too

`make` also makes build/ the game folder: the MSYS2 DLLs the executable needs beside it (tools/bundle_dlls.py
follows the import tables; ~100 DLLs, most of them FFmpeg's; never vulkan-1.dll: the driver's loader must be used)
and the game files in build/data/exefs + build/data/romfs (target `data`, once: hard links to GAME's files, so no
second copy on disk; plain copies where hard links don't work). Started without a game folder argument, the
executable works in its own folder (runtime/main.c, platform plat_standalone): data/ for the game, cache/, mods/,
shots/, mhgurecomp.ini and log.txt beside it (save data elsewhere, see below); with a console of its own (Explorer, a shortcut) it drops the
console and logs to log.txt, and a fatal error shows a message box. `make clean` keeps all that (it removes the
compiled files only). With a game folder argument (or MHGU_GAME_DIR) it uses the current folder as before; then the
DLLs must be on PATH (C:\msys64\ucrt64\bin) unless it is the bundled build/mhgurecomp.exe.

Save data: the standalone executable keeps saves in the player's own folder, %APPDATA%\MHGURecomp\save\ (save_0/
system + system_backup, sdcard/), so deleting or rebuilding build/ can't take them along (platform plat_user_dir;
the path is in the log: "[fs] mounted host directory"). With a game folder argument (the test scripts in run/) saves
stay in save/ under the current folder, so automated runs never touch the real save. MHGU_SAVE_DIR overrides both.
Linux (not written yet): plat_user_dir should return $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp.
First start without a save (checked 2026-10-21 with an empty save_0): the game shows "There is no data to load." and
then the language / bonus notices; these boxes have no button prompt and only A (keyboard L) closes them, B and Plus
do nothing, so it looks frozen. After that: title, New Game; a fresh system + system_backup is written by then.

Mods (runtime/mods.c, mods.h; tested 2026-10-21 with GU Rebalanced): drop a mod's folder into build\mods\ (the
standalone build creates the empty folder), remove the folder to uninstall; nothing on disk is changed, all is layered
at start. romfs/ is found up to three levels down (Mod/Mod/romfs, atmosphere/contents/<title id>/romfs) or as a bare
nativeNX/ folder; names match in any case; new files are added (GU Rebalanced adds weapon/w11/163.arc). exefs/:
IPSwitch .pchtxt and IPS / IPS32 (<build id>.ips) patches, matched to main / sdk by build id; offsets are the module's
memory offsets (pchtxt with offset_shift 0x100, as all of theboy181's: 0xBBCD34 = runtime 0xdbcd34). Only data patches
(rodata / data) are applied; code patches can't be (the code is compiled ahead and literal pools are folded in), the
log lists them: "[mods] <mod>: <file> for main: ... code patch(es) NOT applied (at ...)". The three common MHGU exefs
mods are covered by the port itself: 4K-2K = ini resolution (gen.py PATCHES at the same two instructions), 60fps = ini
frame rate, Connecting-Error-Fix = nifm HLE'd as no internet. A code mod someone needs can become a gen.py PATCHES
entry (or a build option that reads .pchtxt at `make gen`). Order: folder names, or mods\load_order.txt; '_' prefix
skips a folder. Log lines: "[mods] <mod>: romfs <path>", "[romfs] mod <path>: <n> files".

Forge plugins (runtime/forge.c, interp.c; 2026-10-21): Forge (github.com/Fexty12573/forge) is a loader for code mods:
its own module (exefs subsdk9) loads .nro plugins from the RomFS folder nativeNX/plugins/ and gives them hooks,
patches, singletons, save data, log and input. The port doesn't use Forge's module (it patches the console's code);
it finds the plugins itself (game RomFS + every mod's romfs, any case), loads them at 0x03200000 (relocated against
main / sdk, imports of forge_* bound to host code) and runs their code in an AArch32 interpreter (runtime/interp.c:
the lifter's semantics instruction for instruction, same Cpu state; tools/test_lift.py --interp checks it against
Unicorn like the lifter: 0 mismatches over 20k sampled game / SDK instructions incl. NEON + VFP). Calls between the
plugin and the recompiled game go through rt_dispatch both ways. Hooks: every generated function's FN_ENTER (cpu.h)
tests a bit in g_fnhook (one bit per code word, dispatch.c fnhook_set) and runs the detour in the function's place;
the "original" is a trampoline in guest memory with Forge's layout (two original words + `ldr pc` to target+8,
plugins verify it) that dispatch.c runs as the unhooked function. forge_hook_create also writes Forge's `ldr pc,
[pc,#-4]; detour` over the entry (plugins verify that too); patches over code are re-read for that form (or a `b`),
anything else in code is logged as not applicable. Forge's own points: sApp::run (main+0xB8692C: resolve singletons,
forge_onLoad / version check / forge_onInit), the frame update (main+0x3DA8FC: forge_onUpdate(dt)), cSystem ctor /
dtor (singletons). Not provided: the ImGui overlay (forge_onImGui* never called; ig* imports are inert stubs),
forge_graphics_* (report "not initialized"), forge_socket_*. MHGU_FORGE_LOG=debug|info|warn|error (default info).
Install = the mod's folder in build\mods\ (Forge itself is not needed). Log: "[forge] plugin ...", "[forge] starting
...", the plugin's own messages as "[forge] [info] ...".

Bonus content (runtime/services/bonus.c, 2026-10-21): the free downloads (event / challenge quests, item packs,
Palicos) live in the shared part of the save file, not in the game files, and Capcom's server (meteor.capcom.co.jp)
is out of reach. So they are copied from a save that has them: put that save's `system` file in build\bonus\ (next
to the exe, like mods\; the current folder with a game folder argument), else %APPDATA%\MHGURecomp\bonus\ is read;
MHGU_BONUS_DIR overrides. When the game opens a
save whose download regions are empty (0xb2a5-0xb310 unlock flags, 0xc000 to the first character slot: item packs,
Palicos, 160 + 40 quest slots of [id][size][ARC]), the donor's are written in; a save with downloads of its own is
left alone. A brand-new save gets them from the next start (the game writes it without reading it back). Tested with
the user's save (125 event + 40 challenge quests): Low and G Rank Event lists filled (URGENT: COAL MINERS NEEDED in
G Rank), Chamberlyne > Downloadable Content > Claim Items lists the packs and claiming works (Pirate's Booty Pack:
Slickaxe x20). Palico downloads not checked. The quest ARCs equal the RomFS files byte for byte, so a donor-free
version (building the regions from the RomFS) is possible once the flag bits and the pack / Palico records are known.

Controls: see `runtime/host_input.c` (keyboard: WASD / arrows / L K I J = A B X Y / Enter = Plus; SDL gamepads, face
buttons by position (Switch layout) or label: ini FaceButtons;
F12 screenshot, F11 8-frame burst -> build\shots\; Alt+Enter windowed / full screen; F7 cycles MSAA). Display mode,
resolution, anti-aliasing and frame rate: build\mhgurecomp.ini (delete it to get the defaults back).

Debug env: `MHGU_TRACE_SVC`, `MHGU_TRACE_IPC`, `MHGU_LOG_FILES`, `MHGU_FNPROF` (hot functions every 5 s),
`MHGU_FNTRACE=lo-hi[:n]` (calls into a guest address range, with caller and thread), `MHGU_DUMP_THREADS=<s>`
(every guest thread's wait state after s seconds), `MHGU_HLE_SKIP`, `MHGU_AUDIO_LOG`, `MHGU_MOVIE_TRACE` (calls
through the movie vtables), `MHGU_HIDDEN=1` (no visible window), `MHGU_INPUT="frame:BUTTON:frames,..."` (scripted
controller, frames = presented frames; e.g. `300:A:10`), `MHGU_NAME` (hunter name for text entry),
`MHGU_CONTROL=ctl.txt` (drive a running game: append lines `A`, `DOWN 8`, `STICK_UP 60`, `shot name` -> shots/name.png),
`MHGU_SHOT_AT=<frames>`, `MHGU_FPS=1`, `MHGU_VK_STATS=1`, `MHGU_VK_VALIDATION=1`, `MHGU_SYNC_SHADERS=1`, `MHGU_PIPE_WAIT_MS=n`,
`MHGU_NO_PROGRAM_CACHE=1`, `MHGU_SAVE_GLSL=1`.
`python tools/disasm.py <game dir> <addr | module:symbol> [count] [before]` disassembles guest code.

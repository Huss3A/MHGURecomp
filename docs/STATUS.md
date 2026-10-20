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
- Frame rate: MHGU_FRAMERATE (default 60, 30..60) writes the engine's target fps (float at +0x3c of the object in
  global 0x2326764, the field theboy181's 60fps patch sets) each frame. Targets above 60 still run at 60 (another
  cap in the engine, not investigated). MHGU_VSYNC=0/1: no vsync / classic vsync (default mailbox).
- Open: a white flashing bar the user saw in pause menu > Status (not found yet: need a screenshot); lens flare
  blocks when first entering an area (probably draws skipped while shaders compile).

## Next steps

1. Modding tools: decode resident.arc tables (weapon00..14 Base/Level data: record layouts derivable from the game
   code that reads them), monster files per `arc/enemy/emXXX_YY.arc` (`attackdata`, `bodydata`, `dtbase`,
   `dttune`), JSON <-> binary converters, and per-member ARC overrides in the virtual RomFS
   (`mods/<name>/romfs/<arc path>.arc.d/<member>`).
2. Play further (hunts work: Bherna Gal -> quest -> gate left of the balloon) and fix what breaks; shader global
   memory if some effect needs it.
3. A real text entry for the name (SDL text input + an overlay) instead of MHGU_NAME.
4. A README with build instructions; Linux platform layer.
## Build / run

MSYS2 UCRT64 on PATH (`C:\msys64\ucrt64\bin;C:\msys64\usr\bin`):

    make gen GAME=../mhgu/game     # recompile (needs tools/venv: capstone, unicorn, lz4)
    make -j4                       # ~8 min full, incremental (changed chunks only) is fast
    cd run && ../build/mhgurecomp.exe ../../mhgu/game

The executable needs MSYS2's DLLs (SDL3, FFmpeg, winpthread...) on PATH: without them it exits at once with
0xC0000135 and no output.

Controls: see `runtime/host_input.c` (keyboard: WASD / arrows / L K I J = A B X Y / Enter = Plus; SDL gamepads).

Debug env: `MHGU_TRACE_SVC`, `MHGU_TRACE_IPC`, `MHGU_LOG_FILES`, `MHGU_FNPROF` (hot functions every 5 s),
`MHGU_FNTRACE=lo-hi[:n]` (calls into a guest address range, with caller and thread), `MHGU_DUMP_THREADS=<s>`
(every guest thread's wait state after s seconds), `MHGU_HLE_SKIP`, `MHGU_AUDIO_LOG`, `MHGU_MOVIE_TRACE` (calls
through the movie vtables), `MHGU_HIDDEN=1` (no visible window), `MHGU_INPUT="frame:BUTTON:frames,..."` (scripted
controller, frames = presented frames; e.g. `300:A:10`), `MHGU_NAME` (hunter name for text entry),
`MHGU_CONTROL=ctl.txt` (drive a running game: append lines `A`, `DOWN 8`, `STICK_UP 60`, `shot name` -> shots/name.png),
`MHGU_SHOT_AT=<frames>`, `MHGU_FPS=1`, `MHGU_VK_STATS=1`, `MHGU_VK_VALIDATION=1`, `MHGU_SYNC_SHADERS=1`, `MHGU_SAVE_GLSL=1`.
`python tools/disasm.py <game dir> <addr | module:symbol> [count] [before]` disassembles guest code.

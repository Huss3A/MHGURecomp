// Mods: folders under mods/ (beside mhgurecomp.exe in the standalone build, else in the current folder;
// MHGU_MODS_DIR overrides). Dropping a mod's folder in installs it, removing the folder uninstalls it: everything is
// layered over the game's own files at start, nothing is changed on disk. A mod folder may hold:
//   romfs/   loose files layered over the game's RomFS (same paths as in the game's romfs/ folder; a bare nativeNX/
//            folder counts as romfs/nativeNX/)
//   exefs/   IPSwitch patches (*.pchtxt) and IPS / IPS32 patches (<build id>.ips) for the game's code modules.
//            Only patches to data are applied: the port runs code compiled ahead of time (tools/gen.py), so a
//            patched instruction would change nothing; those are listed in the log as not applied.
// These folders are found up to three levels down, so a mod unpacked as Mod/Mod/romfs or with an emulator or
// Atmosphere layout (atmosphere/contents/0100770008DD8000/romfs) works as it is.
// Mods load in name order; a file in a later mod wins. mods/load_order.txt (one folder name per line, '#'
// comments) picks and orders them instead; a folder whose name starts with '_' is skipped.
#pragma once
#include <stdint.h>

struct Module;

const char *mods_dir(void);
void mods_make_dir(void);  // creates an empty mods/ (standalone: shows players where mods go)
int mods_romfs_layers(const char **out, int max);  // host folders to layer over the RomFS, in order
void mods_patch_module(struct Module *m, const uint8_t build_id[32]);  // exefs patches for a loaded module
void forge_init(void);  // Forge plugins from the mods' RomFS (runtime/forge.c): before the game starts

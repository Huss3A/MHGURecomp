// Mods: folders under mods/ (next to the game folder's parent, or MHGU_MODS_DIR). Each mod may hold:
//   romfs/   loose files layered over the game's RomFS (same paths as in the game's romfs/ folder)
// Mods load in name order; a file in a later mod wins. mods/load_order.txt (one folder name per line, '#'
// comments) picks and orders them instead; a folder whose name starts with '_' is skipped.
#pragma once

int mods_romfs_layers(const char **out, int max);  // host folders to layer over the RomFS, in order

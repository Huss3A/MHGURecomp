// MHGU Recomp entry: claim the guest window, load the modules, set up the kernel, run nn::init::Start on the
// main thread.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "hle.h"
#include "kernel.h"
#include "mods.h"

const char *g_game_dir = "game";  // folder holding exefs/ and romfs/ (the player's own dump)

// rtld normally runs every module's DT_INIT (which registers it with musl and runs its init_array) before
// nnMain: nn::init::Start calls its third argument right after nnosInitialize (SDK constructors run early) and its
// fourth after nninitStartup has set up the game's heap.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %x, %u init_array entries)\n", m->name, m->init, m->init_array_sz / 4);
    if (m->init) guest_call(c, m->init);
    else
        for (uint32_t i = 0; i < m->init_array_sz / 4; i++) {
            uint32_t fn = LD32(m->init_array + 4 * i);
            if (fn) guest_call(c, fn);
        }
}
static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// Claim the guest window before anything else can map there.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_BASE, GUEST_SPACE)) return;
    fprintf(stderr, "FATAL: the guest address window at %llx is not free\n", GUEST_BASE);
    exit(1);
}

static void boot(const char *root) {
    char exefs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);

    char why[512];
    if (!gamelib_load(why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    kernel_init();
    dispatch_init();
    forge_init();  // (Forge plugins in the mods: runtime/forge.c)
    hpscale_init();  // (large monster HP by the number of hunters: runtime/hle/hpscale.c)

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint32_t start = loader_find_symbol("_ZN2nn4init5StartEjjPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint32_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint32_t main_init_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, main_init_fn);
}

int main(int argc, char **argv) {
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    bool given = false;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') g_game_dir = argv[i], given = true;
    if (getenv("MHGU_GAME_DIR")) g_game_dir = getenv("MHGU_GAME_DIR"), given = true;
    if (!given) {  // standalone (the build folder): data\ holds the game files; caches, mods and log.txt beside, saves
        plat_standalone();  // in the player's folder (%APPDATA%\MHGURecomp\save)
        g_game_dir = "data";
        mods_make_dir();
        static char save[1024];
        if (plat_user_dir(save, sizeof save - 8)) {
            strcat(save, "/save");
            g_save_dir = save;
        } else {
            fprintf(stderr, "[main] no user data folder: saves go to save\\ beside the executable\n");
        }
        FILE *f = fopen("data/exefs/main", "rb");
        if (!f) {
            const char *msg = "The game files were not found. Put your own dump's exefs and romfs folders in the data "
                              "folder next to mhgurecomp.exe (data\\exefs, data\\romfs).";
            fprintf(stderr, "%s\n", msg);
            plat_error_box(msg);
            return 1;
        }
        fclose(f);
    }
    { void fnprof_init(void); fnprof_init(); }
    crash_handler_install();
    ui_init();
    boot(g_game_dir);
    return ui_run();
}

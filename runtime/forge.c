// Forge plugins (Forge: an MHGU mod loader for code mods, github.com/Fexty12573/forge). A plugin is an AArch32 .nro in
// the RomFS folder nativeNX/plugins/ (so it comes with a mod's romfs, runtime/mods.c); on the console Forge's own
// module (exefs subsdk) loads it and gives it an API: function hooks, code patches, the game's singletons, save data,
// logging, input, an ImGui overlay. The port does not need Forge's module (its hooks rewrite the console's code): it
// loads the plugins itself, runs their code with the interpreter (runtime/interp.c, the plugins call into the
// recompiled game and back) and implements Forge's API on top of the recompiled game, at the points Forge uses:
//   sApp::run          singletons resolved, plugins loaded: forge_onLoad, forge_onInit
//   the frame update   forge_onUpdate(dt) after it, then the ImGui overlay's frame (runtime/overlay.cpp:
//                      forge_onImGuiInit once, forge_onImGuiRender in Forge's window, forge_onImGuiFreeRender)
//   cSystem ctor/dtor  the singletons (forge_singleton_*)
// Hooks run through FN_ENTER (cpu.h, dispatch.c). Patches over data apply as written; a patch over code can only
// redirect a function entry (Forge's own hook form, `ldr pc, [pc, #-4]` + address, or a `b`): others are logged.
// The plugins' ImGui calls (cimgui's ig* / Im*): runtime/forge_imgui.c.
// forge_socket_*: the game's own nn::socket (a virtual network, runtime/net/vsock.c: local play only, no internet).
// Plugins that leave out a required Forge version newer than this one (FORGE_MAJOR.FORGE_MINOR) are rejected (no calls).
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "forge.h"
#include "hle.h"
#include "hle/input.h"
#include "interp.h"
#include "mods.h"
#include "overlay.h"

#define FORGE_MAJOR 1
#define FORGE_MINOR 5
#define FORGE_PATCH 1

// Main module offsets (from its text) Forge uses, MHGU 1.4.0
#define OFF_APP_RUN     0xB8692Cu  // sApp::run
#define OFF_UPDATE      0x3DA8FCu  // the frame update (this: dt at +0x68)
#define OFF_SYSTEM_CTOR 0x887810u  // cSystem::cSystem / ~cSystem: every singleton
#define OFF_SYSTEM_DTOR 0x88784Cu
#define OFF_MOUNT_SAVE  0x878DA0u  // sSavedata's mount
#define OFF_JOB_SAFE    0x212041Cu

#define MAX_PLUGINS (MAX_MODULES - 2)  // g_modules also holds main and sdk
typedef struct {
    char name[256];
    Module *m;
    uint32_t on_load, on_init, on_update;
    uint32_t on_imgui[3];  // forge_onImGuiInit, forge_onImGuiRender, forge_onImGuiFreeRender (FORGE_IMGUI_*)
    uint32_t param;  // PluginInitParam: u16 major, minor, patch; char name[64]
    bool active;  // not rejected (a plugin without forge_onLoad is active too)
} Plugin;
static Plugin plugins[MAX_PLUGINS];
static int nplugins;
static uint32_t main_text;
static int log_level = 1;  // Forge LogLevel: 0 debug, 1 info, 2 warn, 3 error (MHGU_FORGE_LOG)

static uint32_t text_addr(uint32_t off) { return main_text + off; }

// a guest call from host code with up to four word arguments (r0-r3 are scratch here: callers are hooks or HLE
// functions, at a call boundary of the guest)
static uint32_t gcall(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    return c->r[0];
}

static uint32_t gstring(const char *s) {  // a constant string in guest memory
    size_t n = strlen(s) + 1;
    uint32_t a = hle_alloc((uint32_t)n);
    memcpy(G2H(a), s, n);
    return a;
}

// ------------------------------------------------------------------ singletons
// Every cSystem the game makes is noted when its constructor starts (before it has its vtable), and typed once it
// is complete: at sApp::run all of those are (the game's start-up is over), later ones a frame after they were
// noted, and only if what they hold looks like a finished object (a vtable in the game's data whose getDti is code).
#define MAX_SINGLETONS 1024
static struct { uint32_t obj, frame; } pending[MAX_SINGLETONS];
static uint32_t npending;
static struct { uint32_t id, obj; } singletons[MAX_SINGLETONS];
static uint32_t nsingletons;
static pthread_mutex_t singleton_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t frame_no;  // frames updated so far (frame_update)

static uint32_t mt_id(const char *name) {  // MtDti::makeId: CRC-32 without the final inversion, 31 bits
    uint32_t crc = 0xFFFFFFFFu;
    for (; *name; name++) crc = rt_crc32_tab[(crc ^ (uint8_t)*name) & 0xFF] ^ (crc >> 8);
    return crc & 0x7FFFFFFFu;
}

static bool in_code(uint32_t a);

static void system_ctor(Cpu *c) {
    pthread_mutex_lock(&singleton_lock);
    if (npending < MAX_SINGLETONS) {
        pending[npending].obj = c->r[0];
        pending[npending++].frame = __atomic_load_n(&frame_no, __ATOMIC_RELAXED);
    } else {
        LOG("[forge] too many singletons are being made: one is not noted\n");
    }
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_CTOR));
}

static void system_dtor(Cpu *c) {
    uint32_t obj = c->r[0];
    pthread_mutex_lock(&singleton_lock);
    for (uint32_t i = 0; i < npending; i++)
        if (pending[i].obj == obj) pending[i--] = pending[--npending];
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].obj == obj) singletons[i--] = singletons[--nsingletons];
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_DTOR));
}

// MtObject::getDti of a finished object, or 0: the object is not one (yet)
static uint32_t object_dti(Cpu *c, uint32_t obj) {
    if (!kernel_mapped(obj, 4)) return 0;
    uint32_t vt = LD32(obj);
    if (!vt || !kernel_mapped(vt + 0x14, 4)) return 0;
    uint32_t fn = LD32(vt + 0x14);
    if (!in_code(fn)) return 0;
    uint32_t dti = gcall(c, fn, obj, 0, 0, 0);
    return dti && kernel_mapped(dti, 0x20) ? dti : 0;
}

// Type the noted objects that are complete (all = at sApp::run, else the ones a frame old). The others stay noted.
static void singletons_resolve(Cpu *c, bool all) {
    uint32_t list[MAX_SINGLETONS], n = 0, now = __atomic_load_n(&frame_no, __ATOMIC_RELAXED);
    pthread_mutex_lock(&singleton_lock);
    for (uint32_t i = 0; i < npending; i++)
        if (all || pending[i].frame < now) list[n++] = pending[i].obj;
    pthread_mutex_unlock(&singleton_lock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dti = object_dti(c, list[i]);
        if (!dti) continue;
        uint32_t id = LD32(dti + 0x1C);
        pthread_mutex_lock(&singleton_lock);
        uint32_t k = 0, j = 0;
        while (j < npending && pending[j].obj != list[i]) j++;
        if (j == npending) { pthread_mutex_unlock(&singleton_lock); continue; }  // (destroyed meanwhile)
        pending[j] = pending[--npending];
        while (k < nsingletons && singletons[k].id != id) k++;
        if (k < MAX_SINGLETONS) {
            singletons[k].id = id;
            singletons[k].obj = list[i];
            if (k == nsingletons) nsingletons++;
        }
        pthread_mutex_unlock(&singleton_lock);
        if (log_level == 0) LOG("[forge] singleton %s at %08x\n", LD32(dti + 4) ? gstr(LD32(dti + 4)) : "?", list[i]);
    }
}

static uint32_t singleton_by_id(uint32_t id) {
    pthread_mutex_lock(&singleton_lock);
    uint32_t r = 0;
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].id == id) r = singletons[i].obj;
    pthread_mutex_unlock(&singleton_lock);
    return r;
}

static uint32_t singleton(Cpu *c, uint32_t id) {  // (singletons made since sApp::run: typed here once they are finished)
    uint32_t r = singleton_by_id(id);
    if (!r && __atomic_load_n(&npending, __ATOMIC_RELAXED)) {
        singletons_resolve(c, false);
        r = singleton_by_id(id);
    }
    return r;
}
HLE_FUNC("forge_singleton_getInstanceByName", f_singleton_by_name) {
    RET(ARG(0) && kernel_mapped(ARG(0), 1) ? singleton(c, mt_id(gstr(ARG(0)))) : 0);
}
HLE_FUNC("forge_singleton_getInstanceById", f_singleton_by_id) { RET(singleton(c, ARG(0))); }
HLE_FUNC("forge_singleton_getAllInstances", f_singleton_all) {
    uint32_t out = ARG(0), max = ARG(1), n = 0;
    pthread_mutex_lock(&singleton_lock);
    if (!out) n = nsingletons;
    else if (max < 0x10000000u && kernel_mapped(out, 4 * max))
        for (; n < nsingletons && n < max; n++) ST32(out + 4 * n, singletons[n].obj);
    pthread_mutex_unlock(&singleton_lock);
    RET(n);
}

// ------------------------------------------------------------------ hooks and patches
// A hook makes the target's first two words Forge's `ldr pc, [pc, #-4]; .word detour` (plugins check that) and
// installs the detour as a function hook; the original is a trampoline holding the two original words and a jump
// back, as Forge builds it (plugins check that too), which dispatch.c runs as the unhooked function.
static uint32_t tramp_next = FORGE_TRAMP, stub_next = FORGE_STUBS;
static pthread_mutex_t forge_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t t_hook_context;

static bool in_code(uint32_t a) {
    for (int i = 0; i < 2 && i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size) return true;
    }
    return false;
}

// Code patched in guest memory: re-derive the hooks at the function entries in [a, a + n). orig: the bytes before
// (enabling a patch), to report a change that is not a hook.
static void code_changed(uint32_t a, uint32_t n, uint32_t orig) {
    bool hook = false;
    for (uint32_t x = a & ~3u; x < a + n; x += 4) {
        if (!in_code(x) || !dispatch_lookup(x)) continue;
        uint32_t w = LD32(x);
        if (orig && (w == 0xE51FF004u || (w & 0xFF000000u) == 0xEA000000u))  // (the words it replaced)
            fnhook_keep_entry(x, LD32(orig + (x - a)), x + 4 < a + n ? LD32(orig + (x + 4 - a)) : LD32(x + 4));
        if (w == 0xE51FF004u) hook = fnhook_set(x, LD32(x + 4));
        else if ((w & 0xFF000000u) == 0xEA000000u) hook = fnhook_set(x, x + 8 + (uint32_t)((int32_t)(w << 8) >> 6));
        else if (fnhook_get(x)) fnhook_set(x, 0);
    }
    if (!hook && orig && memcmp(G2H(a), G2H(orig), n))
        LOG("[forge] code patch at main+%x (%u bytes) cannot apply to recompiled code: ignored\n", a - main_text, n);
}

// A slot of n bytes in the trampoline or stub region (*next .. end); 0 and a log line when the region is full
static uint32_t slot_alloc(uint32_t *next, uint32_t end, uint32_t n, const char *what) {
    uint32_t a = 0;
    pthread_mutex_lock(&forge_lock);
    if (*next + n <= end) {
        a = *next;
        *next += n;
    }
    pthread_mutex_unlock(&forge_lock);
    if (!a) LOG("[forge] no room left for %s: the hook is not created\n", what);
    return a;
}

// the original of a hook at target: its two words, then a jump back after them (words that read pc would not survive
// the move; Forge's trampoline has the same limit)
static uint32_t make_trampoline(uint32_t slot, uint32_t target) {
    ST32(slot, LD32(target));
    ST32(slot + 4, LD32(target + 4));
    ST32(slot + 8, 0xE51FF004u);  // ldr pc, [pc, #-4]
    ST32(slot + 12, target + 8);
    return slot;
}

// The plugin module whose code holds a, if any (the game's own are modules 0 and 1)
static const Module *plugin_code(uint32_t a) {
    for (int i = 2; i < g_nmodules; i++) {
        const Module *m = &g_modules[i];
        if (a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size) return m;
    }
    return NULL;
}

// A hook inside a function (not at its entry; Forge writes its jump wherever it is asked to): the jump goes into guest
// memory as on the console, and the function holding it runs in the interpreter from then on (dispatch_interpret, as
// for code a cheat changes), which reads that code. The original is a trampoline in interpreted memory.
static bool hook_inside(uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    const Module *m = &g_modules[target >= SDK_BASE];
    uint32_t next, entry = dispatch_function_at(target, &next);
    uint32_t end = next ? next : m->base + m->text_off + m->text_size;
    if (!entry || target + 8 > end) {
        LOG("[forge] hook target %08x is not inside a function of the game: not hooked\n", target);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&stub_next, FORGE_END, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    if (slot) make_trampoline(slot, target);
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    int r = dispatch_interpret(entry, end);
    if (r < 0) LOG("[forge] hook at %s+%x: its function can't run in the interpreter: not hooked\n", m->name,
                   target - m->base);
    else LOG("[forge] hook at %s+%x, inside the function at %s+%x (which runs in the interpreter now) -> %08x\n",
             m->name, target - m->base, m->name, entry - m->base, detour);
    *tramp = slot;
    return true;
}

// A hook in a plugin's own code (a plugin hooking another's function, or its own): the code is interpreted from guest
// memory, so the jump written there is all it takes.
static bool hook_in_plugin(const Module *m, uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    if (target + 8 > m->base + m->text_off + m->text_size) {
        LOG("[forge] hook target %08x is less than two words from the end of %s's code: not hooked\n", target, m->name);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&stub_next, FORGE_END, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    if (slot) make_trampoline(slot, target);
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    LOG("[forge] hook at %s+%x -> %08x\n", m->name, target - m->base, detour);
    *tramp = slot;
    return true;
}

// false: nothing was hooked (logged), whatever the caller's original pointer says; *tramp: the original (0: none wanted)
static bool hook_install(uint32_t target, uint32_t detour, bool want_orig, uint32_t *tramp) {
    *tramp = 0;
    if (target & 3) {
        LOG("[forge] hook target %08x is not a word-aligned ARM address (Thumb is not supported): not hooked\n", target);
        return false;
    }
    if (!detour) {
        LOG("[forge] hook at %08x has no detour: not hooked\n", target);
        return false;
    }
    const Module *pm = plugin_code(target);
    if (pm) return hook_in_plugin(pm, target, detour, want_orig, tramp);
    if (in_code(target) && !dispatch_lookup(target)) return hook_inside(target, detour, want_orig, tramp);
    if (!in_code(target)) {
        LOG("[forge] hook target %08x is not in the code of the game or a plugin: not hooked\n", target);
        return false;
    }
    uint32_t slot = 0;
    if (want_orig && !(slot = slot_alloc(&tramp_next, FORGE_STUBS, 16, "trampolines"))) return false;
    pthread_mutex_lock(&forge_lock);
    uint32_t prev = fnhook_get(target);  // (a hook already there: the original runs it)
    if (slot) {
        make_trampoline(slot, target);
        fnhook_add_trampoline(slot, prev ? prev : target, !prev);
    }
    fnhook_keep_entry(target, LD32(target), LD32(target + 4));  // (interpreted, the function runs its own words)
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    fnhook_set(target, detour);
    if (log_level == 0) LOG("[forge] hook main+%x -> %08x\n", target - main_text, detour);
    *tramp = slot;
    return true;
}

// (debug) Hooks made the way a plugin makes them: MHGU_FORGE_TESTHOOK=<runtime hex address>[,<address>...] hooks there
// at start-up, before the cheats are applied; the control file's "testhook <address>" while the game runs, after them
// (as plugins hook, from forge_onLoad). Each detour is guest code (interpreted), as a plugin's: it logs its first calls
// (an HLE call between saved scratch registers), then at a function's entry calls the original through its trampoline
// and returns (it pushes two words: not for functions with arguments on the stack), inside a function (hook_inside)
// jumps on to it (the jump in is not a call). An entry and an inside hook on one function is what Damage Numbers does.
#define MAX_TEST_HOOKS 4
static struct { uint32_t target; int calls; } tests[MAX_TEST_HOOKS];
static int ntests;

static void test_count(Cpu *c) {
    int i = (int)c->r[0];
    if (i < 0 || i >= ntests) return;
    int n = ++tests[i].calls;
    if (n <= 5 || !(n % 1000)) LOG("[forge] test hook %08x: call %d\n", tests[i].target, n);
}

void forge_test_hook(uint32_t target) {
    target &= ~1u;
    if (!target || ntests == MAX_TEST_HOOKS) return;
    for (int i = 0; i < ntests; i++)
        if (tests[i].target == target) return;
    int i = ntests++;
    tests[i].target = target;
    kernel_map_region(FORGE_TRAMP, FORGE_END - FORGE_TRAMP, 3, 5);  // (no plugins: not mapped yet)
    interp_add_range(FORGE_STUBS, FORGE_END);
    bool entry = dispatch_lookup(target) != NULL;
    uint32_t n = entry ? 11 : 8;
    uint32_t stub = slot_alloc(&stub_next, FORGE_END, 4 * n, "the test hook");
    if (!stub) { ntests--; return; }
    // push {r0-r3, r12, lr}; mov r0, #i; ldr r12, =log; blx r12; pop {r0-r3, r12, lr}; then
    //   entry:  push {r4, lr}; ldr r12, =trampoline; blx r12; pop {r4, pc}; trampoline; log
    //   inside: ldr pc, =trampoline; trampoline; log
    uint32_t code[8] = {0xE92D500F, 0xE3A00000u | (uint32_t)i, entry ? 0xE59FC018 : 0xE59FC00C, 0xE12FFF3C, 0xE8BD500F};
    if (entry) code[5] = 0xE92D4010, code[6] = 0xE59FC004, code[7] = 0xE12FFF3C;
    else code[5] = 0xE51FF004;
    for (uint32_t k = 0; k < (entry ? 8 : 6); k++) ST32(stub + 4 * k, code[k]);
    if (entry) ST32(stub + 32, 0xE8BD8010);
    ST32(stub + 4 * (n - 1), hle_register("forge:test hook", test_count));
    uint32_t tramp;
    if (!hook_install(target, stub, true, &tramp)) { ntests--; return; }
    ST32(stub + 4 * (n - 2), tramp);
    LOG("[forge] test hook %08x (trampoline %08x)\n", target, tramp);
}

static void setcontext(Cpu *c) { t_hook_context = ARG(0); }

// Hook (24 bytes): target, detour, original, context, context cell, has_context (and padding). A failed hook comes
// back with the target and detour and no original (as Forge's does: 0), nothing hooked.
#define HOOK_SIZE 24
static bool hook_args(const char *fn, uint32_t out, uint32_t orig) {
    if (!kernel_mapped(out, HOOK_SIZE) || (orig && !kernel_mapped(orig, 4))) {
        LOG("[forge] %s: the result or the original pointer is not valid memory: not hooked\n", fn);
        return false;
    }
    memset(G2H(out), 0, HOOK_SIZE);
    if (orig) ST32(orig, 0);
    return true;
}

// Hook forge_hook_create(void *target, void *detour, void **original): returned in memory (r0)
HLE_FUNC("forge_hook_create", f_hook_create) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3), tramp;
    if (!hook_args("forge_hook_create", out, orig)) return;
    ST32(out, target), ST32(out + 4, detour);
    if (!hook_install(target, detour, orig != 0, &tramp)) return;
    ST32(out + 8, tramp);
    if (orig) ST32(orig, tramp);
}

// The context stub of forge_hook_createWithContext: Forge's own ARM code (interpreted), which stores the context
// for forge_hook_getContext and goes on to the detour.
HLE_FUNC("forge_hook_createWithContext", f_hook_create_ctx) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3), context = STK(0), tramp;
    if (!hook_args("forge_hook_createWithContext", out, orig)) return;
    ST32(out, target), ST32(out + 4, detour), ST32(out + 12, context), ST8(out + 20, 1);
    uint32_t stub = slot_alloc(&stub_next, FORGE_END, 48, "the context stubs");
    if (!stub) return;
    static const uint32_t code[7] = {0xE92D500F, 0xE59F0010, 0xE5900000, 0xE59FC00C, 0xE12FFF3C, 0xE8BD500F, 0xE59FF004};
    for (int i = 0; i < 7; i++) ST32(stub + 4 * i, code[i]);
    ST32(stub + 28, stub + 40);
    ST32(stub + 32, hle_register("forge_hook_setContextInternal", setcontext));
    ST32(stub + 36, detour);
    ST32(stub + 40, context);
    ST32(out + 16, stub + 40);
    if (!hook_install(target, stub, orig != 0, &tramp)) return;
    ST32(out + 8, tramp);
    if (orig) ST32(orig, tramp);
}

HLE_FUNC("forge_hook_updateContext", f_hook_update_ctx) {
    uint32_t h = ARG(0);
    // (libnx: Module_Libnx 345, LibnxError_BadInput 11)
    if (!kernel_mapped(h, HOOK_SIZE) || !LD8(h + 20) || !LD32(h + 16) || !kernel_mapped(LD32(h + 16), 4)) {
        RET(MAKE_RESULT(345, 11));
        return;
    }
    __atomic_store_n((uint32_t *)G2H(LD32(h + 16)), ARG(1), __ATOMIC_RELEASE);
    ST32(h + 12, ARG(1));
    RET(0);
}

HLE_FUNC("forge_hook_getContext", f_hook_get_ctx) { RET(t_hook_context); }

// Patch: u32 address, size, void *patch_bytes, *original_bytes; bool enabled. The two buffers come from a pool of
// guest memory that takes them back when the patch is destroyed.
#define PATCH_SIZE 20
#define MAX_PATCH 0x100000u
static struct { uint32_t addr, size; } pbuf_free_list[64];
static int npbuf_free;

static uint32_t pbuf_alloc(uint32_t len) {
    uint32_t size = (len + 63) & ~63u;
    pthread_mutex_lock(&forge_lock);
    for (int i = 0; i < npbuf_free; i++)
        if (pbuf_free_list[i].size == size) {
            uint32_t a = pbuf_free_list[i].addr;
            pbuf_free_list[i] = pbuf_free_list[--npbuf_free];
            pthread_mutex_unlock(&forge_lock);
            memset(G2H(a), 0, size);
            return a;
        }
    pthread_mutex_unlock(&forge_lock);
    return hle_alloc(size);
}

static void pbuf_release(uint32_t addr, uint32_t len) {
    pthread_mutex_lock(&forge_lock);
    if (addr && npbuf_free < 64) pbuf_free_list[npbuf_free].addr = addr, pbuf_free_list[npbuf_free++].size = (len + 63) & ~63u;
    pthread_mutex_unlock(&forge_lock);
}

static void patch_apply(uint32_t p, bool on) {
    if (!kernel_mapped(p, PATCH_SIZE)) return;
    uint32_t addr = LD32(p), size = LD32(p + 4), src = LD32(p + (on ? 8 : 12));
    if (!addr || LD8(p + 16) == on) return;
    if (size > MAX_PATCH || !kernel_mapped(addr, size) || !kernel_mapped(src, size)) {
        LOG("[forge] patch at %08x (%u bytes) is not valid memory: not %s\n", addr, size, on ? "enabled" : "disabled");
        return;
    }
    memmove(G2H(addr), G2H(src), size);
    ST8(p + 16, on);
    if (in_code(addr) || in_code(addr + size - 1)) code_changed(addr, size, on ? LD32(p + 12) : 0);
}

HLE_FUNC("forge_patch_create", f_patch_create) {  // (Patch *out, u32 address, const void *bytes, u32 length, bool enable)
    uint32_t out = ARG(0), addr = ARG(1), bytes = ARG(2), len = ARG(3), enable = STK(0) & 0xFF;
    if (!kernel_mapped(out, PATCH_SIZE)) {
        LOG("[forge] forge_patch_create: the result is not valid memory: no patch\n");
        return;
    }
    memset(G2H(out), 0, PATCH_SIZE);
    if (!len) return;
    if (len > MAX_PATCH || !kernel_mapped(addr, len) || !kernel_mapped(bytes, len)) {
        LOG("[forge] forge_patch_create: %u bytes at %08x (from %08x) are not valid memory: no patch\n", len, addr, bytes);
        return;
    }
    uint32_t pb = pbuf_alloc(len), ob = pbuf_alloc(len);
    memcpy(G2H(pb), G2H(bytes), len);
    memcpy(G2H(ob), G2H(addr), len);
    ST32(out, addr), ST32(out + 4, len), ST32(out + 8, pb), ST32(out + 12, ob);
    if (enable) patch_apply(out, true);
}
HLE_FUNC("forge_patch_enable", f_patch_enable) { patch_apply(ARG(0), true); }
HLE_FUNC("forge_patch_disable", f_patch_disable) { patch_apply(ARG(0), false); }
HLE_FUNC("forge_patch_destroy", f_patch_destroy) {
    uint32_t p = ARG(0);
    if (!kernel_mapped(p, PATCH_SIZE) || !LD32(p)) return;
    patch_apply(p, false);
    pbuf_release(LD32(p + 8), LD32(p + 4));
    pbuf_release(LD32(p + 12), LD32(p + 4));
    memset(G2H(p), 0, PATCH_SIZE);
}

// ------------------------------------------------------------------ memory
HLE_FUNC("forge_mem_getMainTextAddr", f_text) { RET(g_modules[0].base + g_modules[0].text_off); }
HLE_FUNC("forge_mem_getMainRodataAddr", f_rodata) { RET(g_modules[0].base + g_modules[0].ro_off); }
HLE_FUNC("forge_mem_getMainDataAddr", f_data) { RET(g_modules[0].base + g_modules[0].data_off); }
HLE_FUNC("forge_mem_getMainBssAddr", f_bss) {
    Module *m = &g_modules[0];
    RET(m->base + m->data_off + m->data_size - m->bss_size);
}
HLE_FUNC("forge_mem_getMainHeapAddr", f_heap) { RET(HEAP_BASE); }

// ------------------------------------------------------------------ log
// printf from the guest: variadic arguments in r2.. (after level and format) and on the stack, 64-bit values in an
// even register pair or an 8-byte aligned stack slot (doubles too: variadic arguments do not use VFP registers)
typedef struct { Cpu *c; int reg; uint32_t stk; } GArgs;
static uint32_t ga32(GArgs *a) {
    if (a->reg < 4) return a->c->r[a->reg++];
    uint32_t v = LD32(a->stk);
    a->stk += 4;
    return v;
}
static uint64_t ga64(GArgs *a) {
    if (a->reg & 1) a->reg++;
    if (a->reg < 4) {
        uint64_t v = a->c->r[a->reg] | ((uint64_t)a->c->r[a->reg + 1] << 32);
        a->reg += 2;
        return v;
    }
    a->reg = 4;
    a->stk = (a->stk + 7) & ~7u;
    uint64_t v = LD64(a->stk);
    a->stk += 8;
    return v;
}

static void gformat(char *out, size_t n, const char *f, GArgs *a) {  // (forge_format)
    size_t o = 0;
    while (*f && o + 1 < n) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = *f++;
        while (*f && strchr("-+ #0", *f) && k < 20) spec[k++] = *f++;
        if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)ga32(a)); f++; }
        else while (isdigit((unsigned char)*f) && k < 24) spec[k++] = *f++;
        if (*f == '.') {
            spec[k++] = *f++;
            if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)ga32(a)); f++; }
            else while (isdigit((unsigned char)*f) && k < 28) spec[k++] = *f++;
        }
        int ll = 0;
        while (*f && strchr("hlzjtL", *f)) { if (*f == 'l' || *f == 'j' || *f == 'L') ll++; f++; }
        char conv = *f ? *f++ : 0;
        char tmp[512];
        if (conv == '%') { out[o++] = '%'; continue; }
        if (strchr("diuxXoc", conv) && conv) {
            uint64_t v = ll >= 2 ? ga64(a) : ga32(a);
            spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
            if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, ll >= 2 ? (long long)v : (long long)(int32_t)v);
            else if (conv == 'c') snprintf(tmp, sizeof tmp, "%c", (int)v);
            else snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
        } else if (conv == 's') {
            spec[k++] = 's', spec[k] = 0;
            uint32_t p = ga32(a);
            snprintf(tmp, sizeof tmp, spec, p ? gstr(p) : "(null)");
        } else if (conv == 'p') {
            snprintf(tmp, sizeof tmp, "0x%x", ga32(a));
        } else if (conv && strchr("fFeEgGaA", conv)) {
            uint64_t bits = ga64(a);
            double d;
            memcpy(&d, &bits, 8);
            spec[k++] = conv, spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
        } else {
            break;
        }
        for (char *t = tmp; *t && o + 1 < n; t++) out[o++] = *t;
    }
    out[o] = 0;
}

void forge_format(char *out, size_t n, const char *fmt, Cpu *c, int reg, uint32_t stk) {
    GArgs a = {c, reg, stk};
    gformat(out, n, fmt, &a);
}

// trace: Forge's forge_log_trace prints whatever the level is (labelled debug)
static void forge_print(int level, const char *fmt, GArgs *a, bool trace) {
    if (level < log_level && !trace) return;
    static const char *names[] = {"debug", "info", "warn", "error"};
    char buf[1024];
    gformat(buf, sizeof buf, fmt, a);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    LOG("[forge] [%s] %s\n", names[level & 3], buf);
}

HLE_FUNC("forge_log", f_log) {
    GArgs a = {c, 2, c->r[13]};
    forge_print((int)ARG(0), gstr(ARG(1)), &a, false);
}
HLE_FUNC("forge_log_trace", f_log_trace) {
    GArgs a = {c, 1, c->r[13]};
    forge_print(0, gstr(ARG(0)), &a, true);
}
HLE_FUNC("forge_log_getLevel", f_log_level) { RET(log_level); }

// ------------------------------------------------------------------ save data
static uint32_t str_savedata;

HLE_FUNC("forge_fs_unmountSaveData", f_unmount_save) {
    bool job_safe = LD8(text_addr(OFF_JOB_SAFE));
    uint32_t save = singleton_by_id(mt_id("sSavedata"));
    if (!save || !LD32(save + 0xF1C)) return;  // not mounted
    gcall(c, guest_sym("_ZN2nn2fs14CommitSaveDataEPKc"), str_savedata, 0, 0, 0);
    bool thread_safe = job_safe || LD8(save + 0x18);
    if (thread_safe) gcall(c, guest_sym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"), save + 4, 0, 0, 0);
    uint32_t n = LD32(save + 0xF1C) - 1;
    ST32(save + 0xF1C, n);
    if (!n) gcall(c, guest_sym("_ZN2nn2fs7UnmountEPKc"), str_savedata, 0, 0, 0);
    if (thread_safe) gcall(c, guest_sym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"), save + 4, 0, 0, 0);
}

HLE_FUNC("forge_fs_mountSaveData", f_mount_save) {
    uint32_t save = singleton(c, mt_id("sSavedata"));
    if (!save) { RET(0); return; }
    if (gcall(c, text_addr(OFF_MOUNT_SAVE), save, 0, 0, 0) != 0) {
        f_unmount_save(c);
        RET(0);
        return;
    }
    RET(1);
}

HLE_FUNC("forge_fs_getSaveDataMountPoint", f_save_mount_point) { RET(str_savedata); }
HLE_FUNC("forge_fs_getRomFsMountPoint", f_romfs_mount_point) {
    static uint32_t s;
    if (!s) s = gstring("app");
    RET(s);
}
HLE_FUNC("forge_fs_fileExists", f_file_exists) {
    uint32_t path = ARG(0), sp = c->r[13];
    c->r[13] = (sp - 16) & ~7u;
    uint32_t handle = c->r[13];
    uint32_t r = gcall(c, guest_sym("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"), handle, path, 1, 0);
    if (!r) gcall(c, guest_sym("_ZN2nn2fs9CloseFileENS0_10FileHandleE"), LD32(handle), 0, 0, 0);
    c->r[13] = sp;
    RET(r == 0);
}

// ------------------------------------------------------------------ nn::os wrappers (same arguments as the SDK's)
#define FORWARD(name, sym, fn)                         \
    HLE_FUNC(name, fn) {                               \
        c->pc = guest_sym(sym);                        \
        rt_dispatch(c);                                \
    }
FORWARD("forge_nnosInitializeLightEvent", "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", f_le_init)
FORWARD("forge_nnosFinalizeLightEvent", "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", f_le_fini)
FORWARD("forge_nnosSignalLightEvent", "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", f_le_signal)
FORWARD("forge_nnosWaitLightEvent", "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", f_le_wait)
FORWARD("forge_nnosTryWaitLightEvent", "_ZN2nn2os17TryWaitLightEventEPNS0_14LightEventTypeE", f_le_trywait)
FORWARD("forge_nnosTimedWaitLightEvent", "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", f_le_timedwait)
FORWARD("forge_nnosClearLightEvent", "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", f_le_clear)
FORWARD("forge_nnosLockMutex", "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", f_mutex_lock)
FORWARD("forge_nnosUnlockMutex", "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", f_mutex_unlock)
HLE_FUNC("forge_nnosInitializeMutex", f_mutex_init) {  // (mutex, recursive): lock level 0
    c->r[2] = 0;
    c->pc = guest_sym("nnosInitializeMutex");
    rt_dispatch(c);
}

// ------------------------------------------------------------------ input (the controller the game sees, the keyboard)
static uint64_t in_now, in_prev;
static uint8_t keys_now[512], keys_prev[512];
static int32_t in_sticks[4];
static bool touch_down;
static float touch_x, touch_y;

static void input_frame(void) {
    in_prev = in_now;
    input_get(&in_now, in_sticks);
    memcpy(keys_prev, keys_now, sizeof keys_now);
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < 512; i++) keys_now[i] = k && i < n ? k[i] : 0;
    // The mouse is the touch screen while it is free (not captured for the camera, Forge's menu closed): the left
    // button is a finger, at the pointer's place on the console's 1280x720 screen.
    SDL_Window *w = ui_window();
    float mx, my;
    int ww = 0, wh = 0;
    touch_down = w && !overlay_menu_open() && !SDL_GetWindowRelativeMouseMode(w) && SDL_GetWindowSize(w, &ww, &wh) &&
                 ww > 0 && wh > 0 && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK);
    if (touch_down) {
        touch_x = fminf(fmaxf(mx * 1280.0f / (float)ww, 0.0f), 1279.0f);
        touch_y = fminf(fmaxf(my * 720.0f / (float)wh, 0.0f), 719.0f);
    }
}

static bool key(const uint8_t *s, uint32_t k) { return k < 512 && s[k]; }
HLE_FUNC("forge_input_isDown", f_in_down) { RET(ARG(0) < 64 && (in_now >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isPressed", f_in_pressed) { RET(ARG(0) < 64 && ((in_now & ~in_prev) >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isReleased", f_in_released) { RET(ARG(0) < 64 && ((~in_now & in_prev) >> ARG(0)) & 1); }
// a controller is connected: a gamepad in use (the keyboard and mouse the game plays with are not one)
HLE_FUNC("forge_input_isConnected", f_in_connected) { RET(input_pad_connected()); }
HLE_FUNC("forge_input_getTouch", f_in_touch) {  // (float *x, float *y): the first finger, in pixels
    if (!touch_down) { RET(0); return; }
    if (ARG(0)) STF(ARG(0), touch_x);
    if (ARG(1)) STF(ARG(1), touch_y);
    RET(1);
}
static void stick(Cpu *c, int i) {
    if (ARG(0)) STF(ARG(0), in_sticks[i] / 32767.0f);
    if (ARG(1)) STF(ARG(1), in_sticks[i + 1] / 32767.0f);
}
HLE_FUNC("forge_input_getStickL", f_in_stick_l) { stick(c, 0); }
HLE_FUNC("forge_input_getStickR", f_in_stick_r) { stick(c, 2); }
HLE_FUNC("forge_input_isKeyDown", f_key_down) { RET(key(keys_now, ARG(0))); }
HLE_FUNC("forge_input_isKeyPressed", f_key_pressed) { RET(key(keys_now, ARG(0)) && !key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isKeyReleased", f_key_released) { RET(!key(keys_now, ARG(0)) && key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isShiftDown", f_shift) { RET(key(keys_now, SDL_SCANCODE_LSHIFT) || key(keys_now, SDL_SCANCODE_RSHIFT)); }
HLE_FUNC("forge_input_isCtrlDown", f_ctrl) { RET(key(keys_now, SDL_SCANCODE_LCTRL) || key(keys_now, SDL_SCANCODE_RCTRL)); }
HLE_FUNC("forge_input_isAltDown", f_alt) { RET(key(keys_now, SDL_SCANCODE_LALT) || key(keys_now, SDL_SCANCODE_RALT)); }
HLE_FUNC("forge_graphics_isInitialized", f_gfx_init) { RET(overlay_enabled()); }
HLE_FUNC("forge_graphics_isMenuOpen", f_gfx_menu) { RET(overlay_menu_open()); }
HLE_FUNC("forge_graphics_isMenuVisible", f_gfx_menu_visible) { RET(overlay_menu_open()); }

// ------------------------------------------------------------------ sockets
// forge_socket_*: Forge's thin wrappers of nn::socket (a Socket is {int fd}, returned in r0), on the port's own
// nn::socket: the virtual network of runtime/net/vsock.c, which carries the game's local play and nothing else. Calls
// that network can't serve fail as Forge's do: -1 (the errno is nn::socket's, from vsock.c).
static void sock_forward(Cpu *c, const char *sym) {  // (the Socket * in r0 becomes its fd)
    HostFn impl = hle_lookup_impl(sym);
    uint32_t p = c->r[0];
    if (!impl || !p || !kernel_mapped(p, 4)) { RET(-1); return; }
    c->r[0] = LD32(p);
    impl(c);
}
#define SOCKET_FWD(name, sym, fn) HLE_FUNC(name, fn) { sock_forward(c, sym); }
SOCKET_FWD("forge_socket_bind", "_ZN2nn6socket4BindEiPK8sockaddrj", f_sock_bind)
SOCKET_FWD("forge_socket_connect", "_ZN2nn6socket7ConnectEiPK8sockaddrj", f_sock_connect)
SOCKET_FWD("forge_socket_listen", "_ZN2nn6socket6ListenEii", f_sock_listen)
SOCKET_FWD("forge_socket_accept", "_ZN2nn6socket6AcceptEiP8sockaddrPj", f_sock_accept)
SOCKET_FWD("forge_socket_shutdown", "_ZN2nn6socket8ShutdownEii", f_sock_shutdown)
SOCKET_FWD("forge_socket_send", "_ZN2nn6socket4SendEiPKvji", f_sock_send)
SOCKET_FWD("forge_socket_recv", "_ZN2nn6socket4RecvEiPvji", f_sock_recv)
SOCKET_FWD("forge_socket_getSockOpt", "_ZN2nn6socket10GetSockOptEiiiPvPj", f_sock_getopt)
SOCKET_FWD("forge_socket_setSockOpt", "_ZN2nn6socket10SetSockOptEiiiPKvj", f_sock_setopt)

HLE_FUNC("forge_socket_create", f_sock_create) {  // (int domain, int type, int protocol) -> Socket
    HostFn impl = hle_lookup_impl("_ZN2nn6socket6SocketEiii");
    if (impl) impl(c);
    else RET(-1);
}
HLE_FUNC("forge_socket_destroy", f_sock_destroy) {  // (Socket *)
    uint32_t p = ARG(0);
    HostFn impl = hle_lookup_impl("_ZN2nn6socket5CloseEi");
    if (!p || !impl || !kernel_mapped(p, 4) || (int32_t)LD32(p) < 0) return;
    c->r[0] = LD32(p);
    impl(c);
    ST32(p, (uint32_t)-1);
}
// The game's sockets are initialized by the game: what Forge's initDefault gets out of the game's own library
HLE_FUNC("forge_socket_initDefault", f_sock_init_default) {
    HostFn impl = hle_lookup_impl("_ZN2nn6socket10InitializeEPvjji");
    if (!impl) { LOG("[forge] forge_socket_initDefault: the port has no nn::socket: failed\n"); RET(-1); return; }
    c->r[0] = c->r[1] = c->r[2] = c->r[3] = 0;
    impl(c);
}
HLE_FUNC("forge_socket_init", f_sock_init) { f_sock_init_default(c); }  // (a plugin's own pool: not needed)
HLE_FUNC("forge_socket_deinit", f_sock_deinit) {}  // (the library is the game's: it stays)

// -mtp=soft code reads its thread pointer through this: a block of the thread's own (a plugin's __thread data)
HLE_FUNC("__aeabi_read_tp", f_read_tp) {
    static __thread uint32_t tp;
    if (!tp) tp = hle_alloc(0x1000);
    RET(tp);
}

// ------------------------------------------------------------------ plugins
void forge_plugins_imgui(Cpu *c, int which, uint32_t a0, uint32_t a1, uint32_t a2) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].active && plugins[i].on_imgui[which]) gcall(c, plugins[i].on_imgui[which], a0, a1, a2, 0);
}

bool forge_plugins_have(int which) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].active && plugins[i].on_imgui[which]) return true;
    return false;
}

int forge_plugin_titles(const char **out, int max) {  // (as Forge's list: the plugins that gave a name)
    int n = 0;
    for (int i = 0; i < nplugins && n < max; i++)
        if (plugins[i].active && plugins[i].param && LD8(plugins[i].param + 6)) out[n++] = gstr(plugins[i].param + 6);
    return n;
}

// A module's initializers, as nn::ro runs them when it loads a module: DT_INIT, then DT_INIT_ARRAY (entries of 0 or -1
// are not functions)
static void plugin_initializers(Cpu *c, Module *m) {
    if (m->init) gcall(c, m->init, 0, 0, 0, 0);
    for (uint32_t k = 0; k < m->init_array_sz / 4; k++) {
        uint32_t fn = LD32(m->init_array + 4 * k);
        if (fn && fn != 0xFFFFFFFFu) gcall(c, fn, 0, 0, 0, 0);
    }
}

// What Forge does in sApp::run (source/main.c): singletons resolved, the library for sockets up, every plugin loaded
// (forge_init did that, with the relocations) and initialized, then each plugin's forge_onLoad, the version check and
// forge_onInit. A plugin with no forge_onLoad gets neither call but stays: it still gets its update and ImGui calls.
static void app_run(Cpu *c) {
    uint32_t args[4];
    memcpy(args, c->r, sizeof args);
    singletons_resolve(c, true);
    f_sock_init_default(c);
    if (c->r[0]) LOG("[forge] failed to initialize the socket library: 0x%X\n", c->r[0]);
    for (int i = 0; i < nplugins; i++) plugin_initializers(c, plugins[i].m);
    for (int i = 0; i < nplugins; i++) {
        Plugin *p = &plugins[i];
        p->param = hle_alloc(72);
        p->active = true;
        if (!p->on_load) {
            LOG("[forge] %s has no forge_onLoad: it only gets its update and ImGui calls\n", p->name);
            continue;
        }
        gcall(c, p->on_load, p->param, 0, 0, 0);
        ST8(p->param + 69, 0);  // (the name is a C string)
        uint32_t major = LD16(p->param), minor = LD16(p->param + 2), patch = LD16(p->param + 4);
        const char *title = gstr(p->param + 6);
        if (major > FORGE_MAJOR || (major == FORGE_MAJOR && minor > FORGE_MINOR)) {
            LOG("[forge] %s needs Forge %u.%u.%u (this is %d.%d.%d): not started\n", p->name, major, minor, patch,
                FORGE_MAJOR, FORGE_MINOR, FORGE_PATCH);
            p->active = false;
            continue;
        }
        LOG("[forge] starting %s%s%s%s\n", p->name, title[0] ? " (" : "", title, title[0] ? ")" : "");
        if (p->on_init) gcall(c, p->on_init, 0, 0, 0, 0);
    }
    memcpy(c->r, args, sizeof args);
    fnhook_call_original(c, text_addr(OFF_APP_RUN));
}

static void frame_update(Cpu *c) {
    uint32_t self = c->r[0];
    fnhook_call_original(c, text_addr(OFF_UPDATE));
    uint32_t ret[2] = {c->r[0], c->r[1]};
    float dt = LDF(self + 0x68), s0 = c->v.f[0];
    __atomic_add_fetch(&frame_no, 1, __ATOMIC_RELAXED);
    input_frame();
    for (int i = 0; i < nplugins; i++) {
        if (!plugins[i].active || !plugins[i].on_update) continue;
        c->v.f[0] = dt;  // (plugins are softfp, as Forge: the float in r0; s0 too, for any built hard-float)
        memcpy(&c->r[0], &dt, 4);
        guest_call(c, plugins[i].on_update);
    }
    overlay_frame(c);
    c->r[0] = ret[0], c->r[1] = ret[1], c->v.f[0] = s0;
}

// a path under dir, matched without regard to case (mods come from every kind of system); false if missing
static bool ci_path(const char *dir, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s", dir);
    char part[256];
    while (*rel) {
        size_t k = strcspn(rel, "/");
        snprintf(part, sizeof part, "%.*s", (int)k, rel);
        rel += k + (rel[k] == '/');
        DIR *d = opendir(out);
        if (!d) return false;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, part)) {
                size_t len = strlen(out);
                snprintf(out + len, n - len, "/%s", e->d_name);
                found = true;
                break;
            }
        closedir(d);
        if (!found) return false;
    }
    return true;
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

// One line of an INI file as Forge reads it (iniparser): "[section]" sets *section; "key = value" gives the trimmed key
// and value (a ';' or '#' ends the value; quotes around it go). false: the line is neither or has no key.
static bool ini_line(char *s, char section[32], char **key, char **val) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '[') {
        char *e = strchr(s, ']');
        size_t n = e ? (size_t)(e - s - 1) : 0;
        snprintf(section, 32, "%.*s", (int)(n < 31 ? n : 31), s + 1);
        return false;
    }
    char *eq = strchr(s, '=');
    if (*s == ';' || *s == '#' || !eq) return false;
    *eq = 0;
    char *v = eq + 1, *end = v + strcspn(v, ";#");
    *end = 0;
    while (end > v && isspace((unsigned char)end[-1])) *--end = 0;
    for (end = eq; end > s && isspace((unsigned char)end[-1]); ) *--end = 0;
    while (isspace((unsigned char)*v)) v++;
    if (*v == '"' && end > v && v[strlen(v) - 1] == '"') v[strlen(v) - 1] = 0, v++;
    *key = s, *val = v;
    return *s != 0;
}

static int log_level_of(const char *s) {  // Forge's names; -1: none of them
    static const char *names[] = {"debug", "info", "warn", "error"};
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(s, names[i])) return i;
    return -1;
}

// The ImGui overlay and the log, with Forge's settings (its config.ini: [menu] key / font_size, [log] level) and font
// where a mod has them (Forge's own files: romfs nativeNX/forge/), else Forge's defaults (Insert, 12 px, info) and
// ImGui's font. MHGU_FORGE_LOG, when set, picks the log level over the file.
static void overlay_start(const char **layers, int nl) {
    int key = 73;  // (a HID usage, as SDL's scancodes: Insert)
    float size = 12.0f;
    int level = -1;
    char path[1400], font[1400] = "";
    for (int l = 0; l < nl; l++) {
        if (ci_path(layers[l], "nativeNX/forge/fonts/Roboto-Medium.ttf", path, sizeof path)) snprintf(font, sizeof font, "%s", path);
        if (!ci_path(layers[l], "nativeNX/forge/config.ini", path, sizeof path)) continue;
        FILE *f = fopen(path, "r");
        char line[256], section[32] = "";
        while (f && fgets(line, sizeof line, f)) {
            char *k, *v;
            if (!ini_line(line, section, &k, &v)) continue;
            if (!strcasecmp(section, "menu") && !strcasecmp(k, "key")) key = (int)strtol(v, NULL, 0);
            else if (!strcasecmp(section, "menu") && !strcasecmp(k, "font_size")) size = (float)atof(v);
            else if (!strcasecmp(section, "log") && !strcasecmp(k, "level") && log_level_of(v) >= 0) level = log_level_of(v);
        }
        if (f) fclose(f);
    }
    if (level >= 0 && !getenv("MHGU_FORGE_LOG")) log_level = level;
    overlay_init(font[0] ? font : NULL, size, key);
}

void forge_init(void) {
    const char *lv = getenv("MHGU_FORGE_LOG");  // debug, info, warn, error
    if (lv && log_level_of(lv) >= 0) log_level = log_level_of(lv);
    main_text = g_modules[0].base + g_modules[0].text_off;
    for (const char *s = getenv("MHGU_FORGE_TESTHOOK"); s && *s; s += *s == ',') {
        char *e;
        forge_test_hook((uint32_t)strtoul(s, &e, 16));
        if (e == s) break;
        s = e;
    }
    // the plugin files: the game's RomFS, then each mod's (a later one replaces one of the same name)
    static char found[MAX_PLUGINS][1400];  // (kept off the stack)
    char base[1024], dir[1100];
    const char *layers[65];
    snprintf(base, sizeof base, "%s/romfs", g_game_dir);
    layers[0] = base;
    int nl = 1 + mods_romfs_layers(layers + 1, 64), nf = 0;
    for (int l = 0; l < nl; l++) {
        if (!ci_path(layers[l], "nativeNX/plugins", dir, sizeof dir)) continue;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 5 || strcasecmp(e->d_name + len - 4, ".nro")) continue;
            int i = 0;
            while (i < nf && strcasecmp(strrchr(found[i], '/') + 1, e->d_name)) i++;
            if (i == MAX_PLUGINS) {
                LOG("[forge] %s: skipped, at most %d plugins can be loaded (%s)\n", e->d_name, MAX_PLUGINS, dir);
                continue;
            }
            snprintf(found[i], sizeof found[i], "%s/%s", dir, e->d_name);
            if (i == nf) nf++;
        }
        if (d) closedir(d);
    }
    if (!nf) return;

    main_text = g_modules[0].base + g_modules[0].text_off;
    str_savedata = gstring("savedata");
    uint32_t at = PLUGIN_BASE;
    for (int i = 0; i < nf; i++) {
        size_t len;
        uint8_t *data = read_all(found[i], &len);
        const char *name = strrchr(found[i], '/') + 1;
        uint32_t bss = 0;
        if (data && len >= 0x3C) memcpy(&bss, data + 0x38, 4);
        // (the image, its bss and the 64 KiB alignment of the next one; the trampolines and stubs have their own regions)
        bool room = data && at + len + bss + 0x10000 <= PLUGIN_END;
        Module *m = room ? loader_load_nro(name, data, len, at) : NULL;
        free(data);
        if (!m) {
            if (data && !room) LOG("[forge] %s: skipped, no room left in the plugin memory region (%s)\n", name, found[i]);
            else LOG("[forge] %s: not a plugin this port can load (%s)\n", name, found[i]);
            continue;
        }
        Plugin *p = &plugins[nplugins++];
        snprintf(p->name, sizeof p->name, "%s", name);
        p->m = m;
        p->on_load = loader_module_symbol(m, "forge_onLoad");
        p->on_init = loader_module_symbol(m, "forge_onInit");
        p->on_update = loader_module_symbol(m, "forge_onUpdate");
        p->on_imgui[FORGE_IMGUI_INIT] = loader_module_symbol(m, "forge_onImGuiInit");
        p->on_imgui[FORGE_IMGUI_RENDER] = loader_module_symbol(m, "forge_onImGuiRender");
        p->on_imgui[FORGE_IMGUI_FREE_RENDER] = loader_module_symbol(m, "forge_onImGuiFreeRender");
        interp_add_range(m->base + m->text_off, m->base + m->text_off + m->text_size);
        kernel_map_region(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        kernel_map_region(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        kernel_map_region(m->base + m->data_off, m->size - m->data_off, 4, 3);
        LOG("[forge] plugin %s (%s)\n", name, found[i]);
        at = (at + m->size + 0xFFFF) & ~0xFFFFu;
    }
    if (!nplugins) return;
    kernel_map_region(FORGE_TRAMP, FORGE_STUBS - FORGE_TRAMP, 3, 5);
    kernel_map_region(FORGE_STUBS, FORGE_END - FORGE_STUBS, 3, 5);
    interp_add_range(FORGE_STUBS, FORGE_END);
    fnhook_set(text_addr(OFF_SYSTEM_CTOR), hle_register("forge:cSystem::cSystem", system_ctor));
    fnhook_set(text_addr(OFF_SYSTEM_DTOR), hle_register("forge:cSystem::~cSystem", system_dtor));
    fnhook_set(text_addr(OFF_APP_RUN), hle_register("forge:sApp::run", app_run));
    fnhook_set(text_addr(OFF_UPDATE), hle_register("forge:update", frame_update));
    overlay_start(layers, nl);
}

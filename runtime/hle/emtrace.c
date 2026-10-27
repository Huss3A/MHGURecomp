// (debug) MHGU_EMTRACE=1: logs every large monster action of the loaded zone with how long it lasted, in real time and
// in game updates, for comparing a monster's moves at different frame rates. MHGU_EMTRACE=2 also logs the monster's
// position every update. MHGU_CONTROL's "emact <group> <index> [mode]" starts that action on the zone's first large
// monster (as its AI does: f_276154).
// Hooked: the engine's frame function (0xda5824: the limiter, which also measures the frame step), after it runs.
#include <math.h>
#include <stdlib.h>

#include "../hle.h"

#define FN_FRAME      0xda5824u   // engine frame start: sleeps to the target, sets the step (+0x68)
#define FN_SET_ACTION 0x276154u   // (monster, group, index, mode): starts a monster action
#define G_SYSTEM      0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define G_SENEMY      0x1a51fe0u  // sEnemy *: +0x1c: the 10 live enemies of the loaded zone
#define EN_INFO       0x1428u     // enemy: status block pointer (+0x40 position, +0x1b6 network id)
#define EN_ACTION     0x73e0u     // enemy: the current action (group, index, ...)
#define MAX_EN        10

static int g_level;
static struct { uint32_t en, act; uint64_t start_ns; uint32_t updates; float x0, y0, z0, rise, dist; } g_seen[MAX_EN];
static volatile int g_req;  // emact request from the control file
static uint32_t g_req_group, g_req_index, g_req_mode;
// emsweep <group> <first> <last>: each action of the group in turn, the next one a second after the last one ended
static volatile int g_sweep;
static uint32_t g_sweep_group, g_sweep_next, g_sweep_last, g_sweep_act;
static uint64_t g_sweep_wait_ns, g_sweep_end_ns;
static uint32_t g_forced = ~0u;  // the last action started from the control file (group | index << 8)

void emtrace_request(uint32_t group, uint32_t index, uint32_t mode) {
    g_req_group = group, g_req_index = index, g_req_mode = mode;
    __atomic_store_n(&g_req, 1, __ATOMIC_RELEASE);
}

void emtrace_sweep(uint32_t group, uint32_t first, uint32_t last) {
    g_sweep_group = group, g_sweep_next = first, g_sweep_last = last, g_sweep_act = 0, g_sweep_wait_ns = 0;
    __atomic_store_n(&g_sweep, 1, __ATOMIC_RELEASE);
}

static bool large(uint32_t en) {
    uint32_t info = LD32(en + EN_INFO);
    if (!info) return false;
    uint32_t id = LD16(info + 0x1b6);
    return id != 0xffff && id < 10000;
}

static void frame(Cpu *c) {
    fnhook_call_original(c, FN_FRAME);
    uint32_t sen = LD32(G_SENEMY), sys = LD32(G_SYSTEM);
    if (!sen) return;
    uint64_t now = plat_now_ns();
    float step = sys ? LDF(sys + 0x68) : 0;
    for (int k = 0; k < MAX_EN; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k);
        if (!en || !large(en)) continue;
        int s = 0;
        while (s < MAX_EN && g_seen[s].en && g_seen[s].en != en) s++;
        if (s == MAX_EN) continue;
        uint32_t act = LD32(en + EN_ACTION), info = LD32(en + EN_INFO);
        float x = LDF(info + 0x40), y = LDF(info + 0x44), z = LDF(info + 0x48);
        bool restart = false;
        if (g_seen[s].en != en) {
            g_seen[s].en = en, restart = true;
            LOG("[em] %08x (vtable %08x) in the zone, action %08x\n", en, LD32(en), act);
        } else if (act != g_seen[s].act) {
            LOG("[em] %08x action %08x -> %08x after %.0f ms, %u updates (step %.3f), rose %.1f, moved %.1f\n", en,
                g_seen[s].act, act, (double)(now - g_seen[s].start_ns) / 1e6, g_seen[s].updates, step, g_seen[s].rise,
                g_seen[s].dist);
            restart = true;
        }
        if (restart) {
            g_seen[s].act = act, g_seen[s].start_ns = now, g_seen[s].updates = 0;
            g_seen[s].x0 = x, g_seen[s].y0 = y, g_seen[s].z0 = z, g_seen[s].rise = 0, g_seen[s].dist = 0;
        }
        if (y - g_seen[s].y0 > g_seen[s].rise) g_seen[s].rise = y - g_seen[s].y0;
        static int dumps;  // level 3: the monster object on 4 updates in a row once it is 200 units up in the air
        if (g_level >= 3 && dumps < 4 && (act & 0xffff) == g_forced && (dumps || y - g_seen[s].y0 > 200)) {
            char path[512];
            const char *home = getenv("MHGU_HOME");
            snprintf(path, sizeof path, "%s%semair_%d.bin", home ? home : "", home ? "/" : "", dumps++);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(G2H(en), 1, 0xd000, f), fclose(f);
            LOG("[em] %s: %08x at height %.1f, step %.3f\n", path, en, y - g_seen[s].y0, step);
        }
        float dx = x - g_seen[s].x0, dz = z - g_seen[s].z0;
        g_seen[s].dist = sqrtf(dx * dx + dz * dz);
        g_seen[s].updates++;
        if (g_level >= 2)
            LOG("[em] %08x %.1f ms act %08x pos %.1f %.1f %.1f step %.3f\n", en, (double)(now - g_seen[s].start_ns) / 1e6,
                act, x, y, z, step);
    }
    if (__atomic_load_n(&g_sweep, __ATOMIC_ACQUIRE) && !g_req) {
        uint32_t en = 0;
        for (int k = 0; k < MAX_EN && !en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k);
            if (e && large(e)) en = e;
        }
        uint32_t cur = en ? (LD32(en + EN_ACTION) & 0xffff) : 0;
        if (g_sweep_act && (cur != g_sweep_act || now >= g_sweep_end_ns)) {  // the swept action ended (or ran 12 s)
            if (cur == g_sweep_act) LOG("[em] sweep: %u:%u still running after 12 s, next\n", cur & 0xff, cur >> 8);
            g_sweep_act = 0;
            g_sweep_wait_ns = now + 1500000000ull;
        } else if (!g_sweep_act && now >= g_sweep_wait_ns) {
            if (g_sweep_next > g_sweep_last) {
                g_sweep = 0;
                LOG("[em] sweep done\n");
            } else {
                g_req_group = g_sweep_group, g_req_index = g_sweep_next++, g_req_mode = 0, g_req = 1;
                g_sweep_act = g_req_group | g_req_index << 8;
                g_sweep_end_ns = now + 12000000000ull;
            }
        }
    }
    if (__atomic_load_n(&g_req, __ATOMIC_ACQUIRE)) {
        g_req = 0;
        uint32_t en = 0;
        for (int k = 0; k < MAX_EN && !en; k++) {
            uint32_t e = LD32(sen + 0x1c + 4u * (uint32_t)k);
            if (e && large(e)) en = e;
        }
        if (!en) { LOG("[em] emact: no large monster in the zone\n"); return; }
        LOG("[em] emact %08x %u:%u mode %u\n", en, g_req_group, g_req_index, g_req_mode);
        g_forced = g_req_group | g_req_index << 8;
        uint32_t saved[16];
        for (int i = 0; i < 16; i++) saved[i] = c->r[i];
        c->r[0] = en, c->r[1] = g_req_group, c->r[2] = g_req_index, c->r[3] = g_req_mode;
        guest_call(c, FN_SET_ACTION);
        for (int i = 0; i < 16; i++) c->r[i] = saved[i];
    }
}

static void set_action(Cpu *c) {
    if (large(c->r[0]))
        LOG("[em] %08x set action %u:%u mode %u (was %08x) lr %08x\n", c->r[0], c->r[1] & 0xff, c->r[2] & 0xff, c->r[3],
            LD32(c->r[0] + EN_ACTION), c->r[14]);
    fnhook_call_original(c, FN_SET_ACTION);
}

void emtrace_init(void) {
    const char *e = getenv("MHGU_EMTRACE");
    if (!e || !*e || !(g_level = atoi(e))) return;
    fnhook_set(FN_FRAME, hle_register("emtrace:frame", frame));
    fnhook_set(FN_SET_ACTION, hle_register("emtrace:set_action", set_action));
    LOG("[em] tracing large monster actions (level %d)\n", g_level);
}

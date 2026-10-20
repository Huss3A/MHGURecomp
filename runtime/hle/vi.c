// nn::vi (display / layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;
uint32_t g_native_window;

// Frame rate. The engine paces itself (frame limiter at main 0xda5d00: sleeps until a frame period has passed, then
// derives its frame delta, which game logic scales by): the target is a float at +0x3c of its main system object
// (pointer in the global at 0x2326764, the same field theboy181's 60fps patch sets). The game puts 30.0 there at
// start and resets it at some transitions, so every presented frame writes the chosen rate back.
// The settings' FPS (mhgurecomp.ini: 30 or 60, default 60, the patch-proven rate); MHGU_FRAMERATE=30..60 overrides
// it. Higher targets were tried (150): the engine still stops at 60 somewhere else, so values above 60 are clamped.
#define FPS_OBJ_GLOBAL 0x2326764u
static float framerate(void) {
    static float f = -1;
    if (f < 0) {
        const char *e = getenv("MHGU_FRAMERATE");
        f = e ? (float)atof(e) : (float)g_config.fps;
        if (!(f >= 30.0f)) f = 30.0f;
        if (f > 60.0f) f = 60.0f;
        LOG("[vi] frame rate target %.0f\n", f);
    }
    return f;
}
void vi_apply_framerate(void) {  // (also called per presented frame: runtime/nvn/vulkan/device.c)
    float want = framerate();
    uint32_t obj = LD32(FPS_OBJ_GLOBAL);
    if (obj < HEAP_BASE || obj >= HEAP_BASE + HEAP_MAX) return;  // (not created yet)
    float cur = LDF(obj + 0x3c);
    if (cur == 30.0f && want != 30.0f) STF(obj + 0x3c, want);
}

// The display's vsync event: 60 Hz, or the frame rate target when that is higher (the game waits for it per frame).
static void *vsync_thread(void *arg) {
    (void)arg;
    float fr = framerate();
    const uint64_t period = (uint64_t)(1e9 / (fr > 60.0f ? fr : 60.0f));
    uint64_t next = plat_now_ns();
    for (;;) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) next = now;  // don't burst-signal to catch up
        vi_apply_framerate();
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}
HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", vi_list_displays) {  // DisplayInfo: name[64], ...
    if (ARG(0) && (int32_t)ARG(1) > 0) {
        memset(ARGP(0), 0, 0x60);
        strcpy((char *)ARGP(0), "Default");
        RET(1);
    } else {
        RET(0);
    }
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { RET(0); }
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling) { RET(0); }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = hle_alloc(0x100);
    ST32(ARG(0), g_native_window);
    RET(0);
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->r[1] = vsync_r;
    c->r[2] = 0;
    c->r[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    RET(0);
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvj", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_dt_allocator) { (void)c; }

// nn::vi (display / layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;
uint32_t g_native_window;

static void *vsync_thread(void *arg) {
    (void)arg;
    const uint64_t period = 16666667;
    uint64_t next = plat_now_ns();
    for (;;) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) next = now;  // don't burst-signal to catch up
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

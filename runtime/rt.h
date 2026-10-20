// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- guest address space (32-bit; host address = GUEST_BASE + guest address)
// Layout of the process as this runtime's kernel presents it (GetInfo / QueryMemory agree with it). The modules
// sit low in the code region as Horizon loads them; the heap, alias and stack regions follow.
#define MAIN_BASE     0x00200000u
#define SDK_BASE      0x02400000u   // (main's image ends at 0x02347000)
#define HLE_BASE      0x03000000u   // fake addresses of host-implemented functions (16 bytes each)
#define HLE_SIZE      0x00100000u
#define TLS_BASE      0x03100000u   // thread local regions, 0x200 bytes per thread
#define TLS_SIZE      0x00100000u
#define HLE_HEAP      0x04000000u   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x04000000u
#define CODE_END      0x08000000u
#define HEAP_BASE     0x08000000u   // svcSetHeapSize
#define HEAP_MAX      0xA0000000u   // 2.5 GB
#define ALIAS_BASE    0xA8000000u   // svcMapMemory targets
#define ALIAS_SIZE    0x28000000u
#define STACK_BASE    0xD0000000u   // stack region (main thread stack, mapped thread stacks)
#define STACK_SIZE    0x20000000u
#define GUEST_SPACE   0x100000000ull

// ---- loader.c
typedef struct Module {
    const char *name;
    uint32_t base, size;
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint32_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint32_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[4];
extern int g_nmodules;
void loader_load(const char *exefs_dir);
uint32_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint32_t addr, uint32_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint32_t addr);
uint32_t hle_register(const char *name, HostFn fn);  // returns a fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint32_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
void crash_handler_install(void);

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
KThread *kernel_create_main_thread(uint32_t entry, uint32_t stack_size, int prio);
void kernel_run_main(KThread *t, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- ui (window loop, runtime/ui.c; host controls, runtime/host_input.c)
struct SDL_Window;
void ui_init(void);                      // SDL and the game window, before the guest starts (the renderer uses it)
int ui_run(void);                        // the host main thread's event loop
struct SDL_Window *ui_window(void);
void ui_toggle_fullscreen(void);         // Alt+Enter: windowed <-> full screen
extern volatile uint64_t g_present_count;  // frames the game presented (the renderer counts them)
union SDL_Event;
void host_input_event(const union SDL_Event *e);
void host_input_update(void);            // recompute the controller state the game reads (each loop pass)

// ---- helpers
void guest_call(Cpu *c, uint32_t addr);   // call a guest function (saves / restores lr)
uint32_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
uint32_t hle_alloc(uint32_t size);        // zeroed guest memory owned by the host (never freed): guest address
extern const char *g_game_dir;            // folder holding exefs/ and romfs/
void rt_unwind_point(void);
extern volatile int g_rt_unwind;

// guest strings / pointers
static inline const char *gstr(uint32_t a) { return (const char *)G2H(a); }

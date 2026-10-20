// Host OS layer: everything the runtime needs that differs between Windows and Linux.
// Implementations: runtime/platform/windows.c, runtime/platform/linux.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim the 4 GB guest window at GUEST_BASE (pages are backed lazily, on first touch).
bool plat_reserve_guest(uint64_t base, uint64_t size);
// Make sure host [p, p+size) inside the window is backed before the OS itself writes there (file reads).
void plat_commit(uint64_t host_addr, uint64_t size);
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t host_addr, uint64_t size, int prot);
#define PLAT_PAGE 0x1000ull
// Pages of host [addr, addr+size) inside the window written since the last call for them (the tracking is reset):
// their host addresses into pages (at most max; a full array means "maybe more": treat all as written), *page = the
// tracking granularity. -1: no write tracking.
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t

// ---- process
void plat_init(void);
bool plat_exe_path(char *buf, size_t size);
// Started without a game folder: work in the executable's folder; log to log.txt there when there is no terminal
// (plat_fatal_notice then tells the player about a fatal error).
void plat_standalone(void);
void plat_error_box(const char *msg);
void plat_fatal_notice(void);

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_crash_handler_install(void);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

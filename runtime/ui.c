// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
//   MHGU_WINDOW=WxH   initial window size (default 1280x720)
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "rt.h"

volatile uint64_t g_present_count;
static SDL_Window *window;

SDL_Window *ui_window(void) { return window; }

void ui_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int w = 1280, h = 720;
    const char *size = getenv("MHGU_WINDOW");
    if (size && sscanf(size, "%dx%d", &w, &h) != 2) w = 1280, h = 720;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", w, h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
}

int ui_run(void) {
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            do {
                if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        host_input_update();
    }
}

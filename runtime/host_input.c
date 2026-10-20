// Host controls -> the Switch controller the game reads (runtime/hle/input.c).
//
// Keyboard: WASD left stick, arrow keys right stick (camera), L = A, K = B, I = X, J = Y, Q = L, E = R, Z = ZL,
//   C = ZR, Enter = Plus, Backspace = Minus, T/F/G/H = d-pad up/left/down/right, X = left stick click,
//   V = right stick click. Holding Left Shift halves the left stick (walking). F12 saves a screenshot, F11 the next 8
//   frames (exact rendered pixels, run\shots\), F10 dumps every render pass of the next 2 frames (debug, slow), F9 toggles how colour clears are done (debug).
// Gamepad (SDL): face buttons by position as on a Switch controller (the bottom one is B), or by their labels
//   (settings FaceButtons, runtime/config.c; MHGU_PAD_POSITIONAL=0|1 overrides).
// Scripted input (tests): MHGU_INPUT="frame:BUTTON:frames,..." holds BUTTON from that presented frame for that many
//   frames. BUTTON: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK, or STICK_UP/DOWN/LEFT/RIGHT and
//   RSTICK_UP/DOWN/LEFT/RIGHT (full tilt).
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "hle/input.h"
#include "rt.h"

#define B(x) (1ull << (x))

static bool keys[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;

static const struct { SDL_Scancode sc; int btn; } key_buttons[] = {
    {SDL_SCANCODE_L, BTN_A}, {SDL_SCANCODE_K, BTN_B}, {SDL_SCANCODE_I, BTN_X}, {SDL_SCANCODE_J, BTN_Y},
    {SDL_SCANCODE_Q, BTN_L}, {SDL_SCANCODE_E, BTN_R}, {SDL_SCANCODE_Z, BTN_ZL}, {SDL_SCANCODE_C, BTN_ZR},
    {SDL_SCANCODE_RETURN, BTN_PLUS}, {SDL_SCANCODE_BACKSPACE, BTN_MINUS},
    {SDL_SCANCODE_T, BTN_UP}, {SDL_SCANCODE_F, BTN_LEFT}, {SDL_SCANCODE_G, BTN_DOWN}, {SDL_SCANCODE_H, BTN_RIGHT},
    {SDL_SCANCODE_X, BTN_LSTICK}, {SDL_SCANCODE_V, BTN_RSTICK},
};

// ---- scripted input
typedef struct { uint64_t start, len; uint64_t buttons; int stick, x, y; } ScriptStep;
#define MAX_STEPS 4096
static ScriptStep script[MAX_STEPS];
static int nscript = -1;

static const char *const button_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                           "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// One step: BUTTON (or STICK_<dir> / RSTICK_<dir>) held from presented frame `start` for `len` frames.
static bool add_step(const char *name, uint64_t start, uint64_t len) {
    ScriptStep st = {start, len, 0, -1, 0, 0};
    const char *dir = NULL;
    if (!strncmp(name, "STICK_", 6)) st.stick = 0, dir = name + 6;
    else if (!strncmp(name, "RSTICK_", 7)) st.stick = 1, dir = name + 7;
    if (dir) {
        // UP, DOWN, LEFT, RIGHT or a diagonal (UPLEFT, DOWNRIGHT...)
        int dx = strstr(dir, "LEFT") ? -1 : strstr(dir, "RIGHT") ? 1 : 0, dy = strstr(dir, "DOWN") ? -1 : strstr(dir, "UP") ? 1 : 0;
        int m = dx && dy ? 23170 : 32767;  // (diagonals: full deflection, not beyond the circle)
        st.x = dx * m, st.y = dy * m;
    } else {
        int b = -1;
        for (int i = 0; i < 16; i++) if (!strcmp(name, button_names[i])) b = i;
        if (b < 0) return false;
        st.buttons = B(b);
    }
    if (nscript == MAX_STEPS) {  // (control file sessions: drop the steps that are over)
        int k = 0;
        for (int i = 0; i < nscript; i++) if (script[i].start + script[i].len > g_present_count) script[k++] = script[i];
        nscript = k;
        if (nscript == MAX_STEPS) return false;
    }
    script[nscript++] = st;
    return true;
}

static void script_parse(void) {
    nscript = 0;
    const char *s = getenv("MHGU_INPUT");
    if (!s) return;
    char *copy = strdup(s), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char name[32];
        unsigned long long start, len;
        if (sscanf(tok, "%llu:%31[^:]:%llu", &start, name, &len) != 3 || !add_step(name, start, len))
            LOG("[input] bad MHGU_INPUT step '%s'\n", tok);
    }
    free(copy);
    LOG("[input] scripted input: %d steps\n", nscript);
}

// MHGU_CONTROL=<file>: commands appended to the file while the game runs (one per line), for driving it from outside:
//   <BUTTON> [frames]      press now (default 8 frames); STICK_UP 30 etc. as in MHGU_INPUT
//   shot <name>            save the next presented frame as shots/<name>.png
//   burst <n> <name>       save the next n presented frames as shots/<name>_<i>.png
//   dump [n]               save colour target 0 (or the depth of a depth-only pass) of every render pass of the next
//                          frame (shots/pass_*.png), or of the next n frames (shots/f<i>_pass_*.png for i >= 1)
//   frame                  log the current frame number
char g_shot_request[256];  // (runtime/nvn/vulkan/device.c saves it)
char g_burst_name[200];
int g_burst_left, g_burst_index;
volatile int g_dump_request;
static void control_poll(void) {
    static const char *path = (const char *)1;
    static long done;
    static uint64_t last_ns;
    if (path == (const char *)1) path = getenv("MHGU_CONTROL");
    if (!path) return;
    uint64_t now = plat_now_ns();
    if (now - last_ns < 50000000ull) return;
    last_ns = now;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < done) done = 0;  // (file replaced)
    if (size > done) {
        fseek(f, done, SEEK_SET);
        char line[300];
        while (ftell(f) < size && fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && ftell(f) >= size) break;  // (partial last line: next time)
            done = ftell(f);
            char cmd[32] = "", arg[256] = "", arg2[200] = "";
            if (sscanf(line, "%31s %255s %199s", cmd, arg, arg2) < 1) continue;
            if (!strcmp(cmd, "msaa")) { void vk_set_msaa(uint32_t); vk_set_msaa((uint32_t)atoi(arg)); continue; }
            if (!strcmp(cmd, "gpuprof")) { extern volatile int g_gpuprof_request; g_gpuprof_request = 1; continue; }
            if (!strcmp(cmd, "dump")) { g_dump_request = atoi(arg) > 0 ? atoi(arg) : 1; continue; }
            if (!strcmp(cmd, "memsave")) {  // memsave <guest hex address> <hex bytes> <file>: raw guest memory to a file
                char path[200] = "";
                sscanf(line, "%*s %*s %*s %199s", path);
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16), n = (uint32_t)strtoul(arg2, NULL, 16);
                FILE *o = path[0] && n <= (16u << 20) ? fopen(path, "wb") : NULL;
                if (o) { fwrite(G2H(a), 1, n, o); fclose(o); }
                LOG("[memsave] %08x+%x -> %s%s\n", a, n, path, o ? "" : " (failed)");
                continue;
            }
            if (!strcmp(cmd, "poke")) {  // poke <guest hex address> <hex word>: write guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u, v = (uint32_t)strtoul(arg2, NULL, 16);
                LOG("[poke] %08x: %08x -> %08x\n", a, LD32(a), v);
                ST32(a, v);
                continue;
            }
            if (!strcmp(cmd, "fntrace")) {  // fntrace <lo hex> <hi hex> [n]: log the next n calls of guest functions there
                void fntrace_set(uint32_t, uint32_t, int);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                fntrace_set((uint32_t)strtoul(arg, NULL, 16), (uint32_t)strtoul(arg2, NULL, 16), arg3[0] ? atoi(arg3) : 200);
                continue;
            }
            if (!strcmp(cmd, "peek")) {  // peek <guest hex address> [words]: log guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u;
                int nw = arg2[0] ? atoi(arg2) : 8;
                for (int k = 0; k < nw && k < 256; k += 4) {
                    char line[96];
                    int len = snprintf(line, sizeof line, "[peek] %08x:", a + 4 * k);
                    for (int j = k; j < nw && j < k + 4; j++) len += snprintf(line + len, sizeof line - len, " %08x", LD32(a + 4 * j));
                    LOG("%s\n", line);
                }
                continue;
            }
            if (!strcmp(cmd, "burst")) {
                snprintf(g_burst_name, sizeof g_burst_name, "%s", arg2[0] ? arg2 : "burst");
                g_burst_index = 0;
                g_burst_left = atoi(arg) > 0 ? atoi(arg) : 4;
                continue;
            }
            if (!strcmp(cmd, "shot")) {
                snprintf(g_shot_request, sizeof g_shot_request, "shots/%s.png", arg[0] ? arg : "control");
            } else if (!strcmp(cmd, "frame")) {
                LOG("[control] frame %llu\n", (unsigned long long)g_present_count);
            } else {
                unsigned long n = arg[0] ? strtoul(arg, NULL, 10) : 8;
                if (!add_step(cmd, g_present_count + 1, n ? n : 8)) LOG("[control] unknown command '%s'\n", cmd);
                else LOG("[control] %s %lu at frame %llu\n", cmd, n ? n : 8, (unsigned long long)g_present_count);
            }
        }
    }
    fclose(f);
}
// ---- events
static void open_pad(SDL_JoystickID id) {
    if (pad) return;
    pad = SDL_OpenGamepad(id);
    if (pad) LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
}

void host_input_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // F12: screenshot, F11: 8 consecutive frames (run\shots\)
            static int nshot;
            uint64_t t = (uint64_t)time(NULL);
            if ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT)) {
                keys[e->key.scancode] = false;  // (Alt+Enter: windowed <-> full screen, not a button press)
                ui_toggle_fullscreen();
            } else if (e->key.scancode == SDL_SCANCODE_F12)
                snprintf(g_shot_request, sizeof g_shot_request, "shots/shot_%llu_%d.png", (unsigned long long)t, nshot++);
            else if (e->key.scancode == SDL_SCANCODE_F11) {
                snprintf(g_burst_name, sizeof g_burst_name, "burst_%llu_%d", (unsigned long long)t, nshot++);
                g_burst_index = 0;
                g_burst_left = 8;
            } else if (e->key.scancode == SDL_SCANCODE_F10) {
                g_dump_request = 2;  // (debug) every render pass of the next 2 frames, as "dump 2"
            } else if (e->key.scancode == SDL_SCANCODE_F7) {  // MSAA off / 2x / 4x / 8x (to compare; the settings set it)
                void vk_cycle_msaa(void);
                vk_cycle_msaa();
            } else if (e->key.scancode == SDL_SCANCODE_F8) {  // (debug) shadow-pass scissors: as set / none / mirrored
                extern volatile int g_shadow_scissor_mode;
                g_shadow_scissor_mode = (g_shadow_scissor_mode + 1) % 3;
                LOG("[input] F8: shadow scissor mode %d\n", g_shadow_scissor_mode);
            } else if (e->key.scancode == SDL_SCANCODE_F9) {  // (debug) colour clears: vkCmdClearAttachments <-> draws
                extern volatile int g_clear_draw;
                g_clear_draw = g_clear_draw > 0 ? 0 : 1;
                LOG("[input] F9: colour clears as %s\n", g_clear_draw ? "draws" : "vkCmdClearAttachments");
            }
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        memset(keys, 0, sizeof keys);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = NULL;
            int n = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&n);
            if (n > 0) open_pad(ids[0]);
            SDL_free(ids);
        }
        break;
    }
}

static int32_t clamp_axis(int v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

void host_input_update(void) {
    if (nscript < 0) script_parse();
    control_poll();
    uint64_t b = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;

    for (unsigned i = 0; i < sizeof key_buttons / sizeof *key_buttons; i++)
        if (keys[key_buttons[i].sc]) b |= B(key_buttons[i].btn);
    int kx = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A], ky = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
    int k = keys[SDL_SCANCODE_LSHIFT] ? 16000 : 32767;
    if (kx && ky) k = k * 7071 / 10000;  // diagonals: same length as straight
    lx += kx * k, ly += ky * k;
    rx += (keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT]) * 32767;
    ry += (keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]) * 32767;

    if (pad) {
        static const struct { SDL_GamepadButton sdl; int btn; } pad_buttons[] = {
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R},
            {SDL_GAMEPAD_BUTTON_START, BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LSTICK}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RSTICK},
        };
        for (unsigned i = 0; i < sizeof pad_buttons / sizeof *pad_buttons; i++)
            if (SDL_GetGamepadButton(pad, pad_buttons[i].sdl)) b |= B(pad_buttons[i].btn);
        // face buttons: SDL names them by position (south, east, west, north). Settings FaceButtons: Position = the
        // Switch layout; Label = the letter on the button (PlayStation symbols as an Xbox pad's letters at that place)
        static const SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                                  SDL_GAMEPAD_BUTTON_NORTH};
        static const int switch_at[4] = {BTN_B, BTN_A, BTN_Y, BTN_X}, xbox_at[4] = {BTN_A, BTN_B, BTN_X, BTN_Y};
        for (int i = 0; i < 4; i++) {
            if (!SDL_GetGamepadButton(pad, face[i])) continue;
            int btn = switch_at[i];
            if (g_config.face_buttons == FACE_LABEL) {
                switch (SDL_GetGamepadButtonLabel(pad, face[i])) {
                case SDL_GAMEPAD_BUTTON_LABEL_A: btn = BTN_A; break;
                case SDL_GAMEPAD_BUTTON_LABEL_B: btn = BTN_B; break;
                case SDL_GAMEPAD_BUTTON_LABEL_X: btn = BTN_X; break;
                case SDL_GAMEPAD_BUTTON_LABEL_Y: btn = BTN_Y; break;
                default: btn = xbox_at[i]; break;
                }
            }
            b |= B(btn);
        }
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= B(BTN_ZL);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= B(BTN_ZR);
        // sticks: SDL's y is down; a small dead zone against drift
        int ax[4] = {SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)};
        for (int i = 0; i < 4; i++)
            if (ax[i] > -3000 && ax[i] < 3000) ax[i] = 0;
        lx += ax[0], ly += ax[1], rx += ax[2], ry += ax[3];
    } else {
        int n = 0;  // controllers present at startup send no ADDED event to a loop that started after them
        static bool scanned;
        if (!scanned) {
            scanned = true;
            SDL_JoystickID *ids = SDL_GetGamepads(&n);
            if (n > 0) open_pad(ids[0]);
            SDL_free(ids);
        }
    }

    uint64_t frame = g_present_count;
    for (int i = 0; i < nscript; i++) {
        const ScriptStep *st = &script[i];
        if (frame < st->start || frame >= st->start + st->len) continue;
        b |= st->buttons;
        if (st->stick == 0) lx = st->x, ly = st->y;
        if (st->stick == 1) rx = st->x, ry = st->y;
    }

    input_set(b, clamp_axis(lx), clamp_axis(ly), clamp_axis(rx), clamp_axis(ry));
}

// Player settings (config.h): mhgurecomp.ini next to the executable. A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1
//                        (test runs; override the file)
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

Config g_config = {DISPLAY_BORDERLESS, 1920, 1080, 0, 0, 60, 1, 1};
uint32_t g_render_size[2] = {1920, 1080};

static const char DEFAULT_INI[] =
    "; MHGU Recomp settings. Delete this file to get the defaults back: it is written again at the next start.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed or Borderless (a borderless window covering the whole screen).\n"
    "; Alt+Enter switches between Windowed and the full-screen mode chosen here.\n"
    "Mode = Borderless\n"
    "\n"
    "; Screen size: WxH (1280x720, 1600x900, 1920x1080, 2560x1440...) or Desktop (the monitor's own resolution).\n"
    "; Fullscreen switches the monitor to this mode, Windowed opens a window this size (it can be resized), and\n"
    "; Borderless always covers the screen, scaling the game's image to it.\n"
    "Resolution = 1920x1080\n"
    "\n"
    "; The game's 3D resolution: Auto (as large as fits Resolution at 16:9) or WxH. Lower is faster, higher is sharper;\n"
    "; the Switch renders 1920x1080 docked. Above Resolution it supersamples (smoothest at exactly double, e.g.\n"
    "; 3840x2160 for 1920x1080). The game is made for 16:9, so other shapes are fitted to 16:9.\n"
    "RenderResolution = Auto\n"
    "\n"
    "[Anti-aliasing]\n"
    "; MSAA: Off, 2x, 4x or 8x. Smooths the edges of 3D geometry and of some cut-out leaves and fences; each step\n"
    "; costs GPU time and video memory, a lot on older cards (an R9 270X at 1080p: 56 fps off, 35 at 2x, 28 at 4x).\n"
    "; F7 cycles through them while playing, to compare. (The Switch version has none.)\n"
    "MSAA = Off\n"
    "\n"
    "; FXAA: On or Off. The game's own edge smoothing: a filter over the finished image, slightly blurry.\n"
    "FXAA = On\n"
    "\n"
    "[Performance]\n"
    "; Frame rate cap: 60, or 30 (the Switch version's rate).\n"
    "FPS = 60\n";

static void ini_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) { snprintf(buf, size, "mhgurecomp.ini"); return; }
    char *slash = strrchr(exe, '\\'), *s2 = strrchr(exe, '/');
    if (s2 > slash) slash = s2;
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(buf, size, "%smhgurecomp.ini", exe);
}

// lower case, spaces and '_' / '-' removed: "Borderless Window" == "borderlesswindow"
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') *d++ = (char)tolower((unsigned char)*s);
    *d = 0;
}

static bool parse_size(const char *v, int *w, int *h) {
    int a, b;
    char x;
    if (sscanf(v, "%d %c %d", &a, &x, &b) != 3 || (x != 'x' && x != 'X' && x != '*')) return false;
    if (a < 320 || b < 180 || a > 16384 || b > 16384) return false;
    *w = a, *h = b;
    return true;
}

static int desktop_w, desktop_h;
static bool render_auto = true;

static void set_key(const char *key, char *val, int line) {
    char k[64], v[256];
    snprintf(k, sizeof k, "%s", key);
    snprintf(v, sizeof v, "%s", val);
    squash(k);
    squash(v);
    bool ok = true;
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "fullscreen") || !strcmp(v, "exclusive") || !strcmp(v, "exclusivefullscreen"))
            g_config.display_mode = DISPLAY_FULLSCREEN;
        else if (!strcmp(v, "windowed") || !strcmp(v, "window")) g_config.display_mode = DISPLAY_WINDOWED;
        else if (!strncmp(v, "borderless", 10)) g_config.display_mode = DISPLAY_BORDERLESS;
        else ok = false;
    } else if (!strcmp(k, "resolution")) {
        if (!strcmp(v, "desktop") || !strcmp(v, "auto")) g_config.width = g_config.height = 0;
        else ok = parse_size(v, &g_config.width, &g_config.height);
    } else if (!strcmp(k, "renderresolution")) {
        if (!strcmp(v, "auto")) render_auto = true;
        else if ((ok = parse_size(v, &g_config.render_w, &g_config.render_h))) render_auto = false;
    } else if (!strcmp(k, "msaa")) {
        if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "0") || !strcmp(v, "1") || !strcmp(v, "1x"))
            g_config.msaa = 1;
        else if (!strcmp(v, "2x") || !strcmp(v, "2")) g_config.msaa = 2;
        else if (!strcmp(v, "4x") || !strcmp(v, "4")) g_config.msaa = 4;
        else if (!strcmp(v, "8x") || !strcmp(v, "8")) g_config.msaa = 8;
        else ok = false;
    } else if (!strcmp(k, "fxaa")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fxaa = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fxaa = 0;
        else ok = false;
    } else if (!strcmp(k, "fps") || !strcmp(k, "framerate")) {
        int f = atoi(v);
        if (f >= 30 && f <= 60) g_config.fps = f;
        else ok = false;
    } else {
        LOG("[config] line %d: unknown setting \"%s\" (ignored)\n", line, key);
        return;
    }
    if (!ok) LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// The largest 16:9 size inside w x h, even numbers.
static void fit_16_9(int w, int h, int *rw, int *rh) {
    if ((int64_t)w * 9 > (int64_t)h * 16) w = h * 16 / 9;
    else h = w * 9 / 16;
    *rw = w & ~1, *rh = h & ~1;
}

void config_load(void) {
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *o = fopen(path, "w");
        if (o) { fputs(DEFAULT_INI, o); fclose(o); LOG("[config] wrote the default settings to %s\n", path); }
        else LOG("[config] %s is missing and could not be written; using the defaults\n", path);
    } else {
        char buf[512];
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
    }

    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    desktop_w = dm ? dm->w : 1920, desktop_h = dm ? dm->h : 1080;
    if (!g_config.width) g_config.width = desktop_w, g_config.height = desktop_h;

    const char *e = getenv("MHGU_WINDOW");
    int w, h;
    if (e && parse_size(e, &w, &h)) g_config.display_mode = DISPLAY_WINDOWED, g_config.width = w, g_config.height = h;
    if (getenv("MHGU_HIDDEN")) g_config.display_mode = DISPLAY_WINDOWED;
    if ((e = getenv("MHGU_MSAA"))) g_config.msaa = atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : atoi(e) >= 2 ? 2 : 1;
    if ((e = getenv("MHGU_FXAA"))) g_config.fxaa = atoi(e) != 0;
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;

    if (render_auto) {  // Borderless shows the image at the desktop size: render for the size it ends up shown at
        if (g_config.display_mode == DISPLAY_BORDERLESS)
            fit_16_9(g_config.width < desktop_w ? g_config.width : desktop_w,
                     g_config.height < desktop_h ? g_config.height : desktop_h, &w, &h);
        else
            fit_16_9(g_config.width, g_config.height, &w, &h);
        g_config.render_w = w, g_config.render_h = h;
    } else {
        fit_16_9(g_config.render_w, g_config.render_h, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not 16:9: using %dx%d\n", g_config.render_w, g_config.render_h, w, h);
        g_config.render_w = w, g_config.render_h = h;
    }
    g_render_size[0] = (uint32_t)g_config.render_w;
    g_render_size[1] = (uint32_t)g_config.render_h;
    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d, %d fps, MSAA %dx, FXAA %s\n", path, modes[g_config.display_mode],
        g_config.width, g_config.height, g_config.render_w, g_config.render_h, g_config.fps, g_config.msaa,
        g_config.fxaa ? "on" : "off");
}

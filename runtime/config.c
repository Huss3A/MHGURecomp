// Player settings (config.h): mhgurecomp.ini in the game folder (next to the executable, or MHGU_HOME; with a game
// folder argument, the current folder). A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1,
//                        MHGU_PAD_POSITIONAL=0|1 (test runs; override the file)
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

Config g_config = {DISPLAY_BORDERLESS, 1920, 1080, 0, 0, 60, 1, 1, 1, FACE_POSITION, 39500, "", 0, 1, 1, {{1, 1, 1, 1}, 2}, 1.0f, 0.0f, 1.0f, UI_CENTER, 1, 0, FSR_NATIVE, 0.8f, 1, 0.9f, 1, FSR_UP_3};
uint32_t g_render_size[2] = {1920, 1080};
float g_render_aspect = 16.0f / 9.0f;
float g_fov_scale = 1.0f;
uint32_t g_ui_size[2] = {1280, 720};
float g_ui_anchor_aspect = 16.0f / 9.0f;
uint32_t g_fsr_output[2];
float g_fsr_lod_bias;

static const char DEFAULT_INI[] =
    "; MHGU Recomp settings. Delete this file to reset to default settings, the file is regenerated.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed, Borderless\n"
    "Mode = Borderless\n"
    "\n"
    "; Resolution (WxH)\n"
    "Resolution = 1920x1080\n"
    "\n"
    "; Rendering resolution (WxH)\n"
    "; Higher = sharper (for supersampling)\n"
    "; Default: Auto\n"
    "RenderResolution = Auto\n"
    "\n"
    "; Set to 16:9 to preserve the game's native aspect ratio\n"
    "; Default: Auto\n"
    "AspectRatio = Auto\n"
    "\n"
    "; Field of view\n"
    "; Between 0.5 and 2\n"
    "; Default: 1.0\n"
    "FOV = 1.0\n"
    "\n"
    "; At resolutions wider than 16:9, determines where the HUD goes\n"
    "; Center, Edges\n"
    "; Default: Center\n"
    "UIAnchor = Center\n"
    "\n"
    "[Anti-aliasing]\n"
    "; Multi-Sample Anti Aliasing, cycle modes with F7 key\n"
    "; Off, 2x, 4x, 8x\n"
    "; Default: Off\n"
    "MSAA = Off\n"
    "\n"
    "; GU's default FXAA\n"
    "; Default: On\n"
    "FXAA = On\n"
    "\n"
    "[Upscaler]\n"
    "; FSR temporal upscaling\n"
    "; Default: Off\n"
    "FSR = Off\n"
    "\n"
    "; Which upscaler to use (FSR3 is FSR 3.1)\n"
    "; FSR3, FSR4\n"
    "; Default: FSR3\n"
    "Upscaler = FSR3\n"
    "\n"
    "; FSR quality\n"
    "; NativeAA, Quality, Balanced, Performance, UltraPerformance\n"
    "; Default: NativeAA\n"
    "FSRQuality = NativeAA\n"
    "\n"
    "; FSR's sharpening filter strength\n"
    "; Between 0 to 1\n"
    "; Default: 0.8\n"
    "Sharpness = 0.8\n"
    "\n"
    "; FSR Object Motion, reduces smearing\n"
    "; Default: On\n"
    "ObjectMotion = On\n"
    "\n"
    "; Reactive anti-ghosting\n"
    "; Between 0 and 1\n"
    "; Default: 0.9\n"
    "Reactive = 0.9\n"
    "\n"
    "; Animated surface textures skip FSR's frame history to avoid smearing\n"
    "; Default: On\n"
    "AnimSurf = On\n"
    "\n"
    "[Performance]\n"
    "; Framerate cap\n"
    "; Between 30 and 240\n"
    "; Default: 60\n"
    "FPS = 60\n"
    "\n"
    "; VSync\n"
    "VSync = On\n"
    "\n"
    "[Controls]\n"
    "; Controller face button layout. Set to \"Label\" to reverse A/B and X/Y on XBox controllers to match the button prompts (not recommended)\n"
    "; Position, Label\n"
    "; Default: Position\n"
    "FaceButtons = Position\n"
    "\n"
    "; Controller input is also registered if the window is out of focus\n"
    "; Default: On\n"
    "BackgroundInput = On\n"
    "\n"
    "[Mouse]\n"
    "; Mouse sensitivity\n"
    "; Default: 1.0\n"
    "MouseSensitivity = 1.0\n"
    "\n"
    "[Multiplayer]\n"
    "; Which port to host/join hubs on (TCP and UDP)\n"
    "; Default: 39500\n"
    "Port = 39500\n"
    "\n"
    "; IP Address of the host to join. Use IP:Port if it doesn't match the port above.\n"
    "; Can be multiple IP addresses, comma-separated\n"
    "; Default:\n"
    "Join =\n"
    "\n"
    "; Sync small monsters between players in multiplayer.\n"
    "; (Currently off by default, pending a bug investigation where small monsters may refuse to attack players)\n"
    "; Default: Off\n"
    "SyncSmallMonsters = Off\n"
    "\n"
    "[Monster HP]\n"
    "; Playercount-specific HP multipliers for monsters in Hub quests.\n"
    "; Only affects large monster health.\n"
    "; When joining a hub, the host's scaling settings are always used.\n"
    "; Default: 1.0\n"
    "1Player = 1.0\n"
    "2Players = 1.0\n"
    "3Players = 1.0\n"
    "4Players = 1.0\n"
    "\n"
    "; (Optional: quest-specific HP multipliers go here. Comma-separated per playercount, 1 line per quest. Example:)\n"
    "; Quest10107 = 1.0, 1.0, 1.0, 1.0\n"
    "\n"
    "[Village Quests]\n"
    "; Press L/R in the Hub while selecting a quest to swap to the Village Quest list instead, letting you play Village quests in multiplayer.\n"
    "; Default: On\n"
    "VillageQuestsInHub = On\n"
    "\n"
    "; When playing a Village Quest from the Hub, first multiply the monster HP by this amount.\n"
    "; This amount is then scaled again by the [Monster HP] settings.\n"
    "; Default: 2.0\n"
    "HubHP = 2.0\n"
    "\n"
    "[Bonus Content]\n"
    "; If enabled, the free online downloadable content (Event Quests, item packs, Palicoes) are automatically copied from the provided savegame inside the `bonus/` folder into your own savegame.\n"
    "; This only happens if your savegame hasn't received any downloadable content yet.\n"
    "; Default: On\n"
    "BonusContent = On\n";

static void ini_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    snprintf(buf, size, "mhgurecomp.ini");  // (started on its own the game runs in its folder, or MHGU_HOME: main.c)
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

// [Monster HP] factors: "0.6", "0.6x", or a comma separated list of up to four (squashed: no spaces). The count read,
// 0 when one is not a number in HP_SCALE_MIN..HP_SCALE_MAX.
static int parse_factors(const char *v, float f[4]) {
    int n = 0;
    while (*v && n < 4) {
        char *end;
        float x = strtof(v, &end);
        if (end == v || !(x >= HP_SCALE_MIN && x <= HP_SCALE_MAX)) return 0;
        f[n++] = x;
        v = end + (*end == 'x');
        if (*v == ',') v++;
        else if (*v) return 0;
    }
    return *v ? 0 : n;
}

// The vertical field of view (radians) a camera of this aspect gets (tools/gen.py PATCHES: the three callers of the
// perspective matrix that load a camera's). One made for the screen (its 16:9 aspect replaced with g_render_aspect)
// gets the FOV factor and, on a picture narrower than 16:9 (16:10), the height that keeps 16:9's width (Vert+): more
// above and below instead of less to the sides. Other cameras keep their own.
float camera_fovy(float fovy, float aspect) {
    if (aspect != g_render_aspect) return fovy;
    fovy *= g_fov_scale;
    if (aspect < 16.0f / 9.0f) fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}

// A picture shape: "Auto" (0), "21:9" / "21/9", or a number (0.5 .. 10).
static bool parse_aspect(const char *v, float *out) {
    float n, d, a;
    char sep, *end;
    if (!strcasecmp(v, "auto")) { *out = 0; return true; }
    if (sscanf(v, "%f %c %f", &n, &sep, &d) == 3 && (sep == ':' || sep == '/') && n > 0 && d > 0) {
        a = n / d;
    } else {
        a = strtof(v, &end);
        if (end == v || *end) return false;
    }
    if (a < 0.5f || a > 10.0f) return false;
    *out = a;
    return true;
}

// UIAnchor: Center (also 1, 16:9), 21:9 (2), 32:9 (3) or Edges, as Config.ui_anchor's UI_*
static bool parse_anchor(const char *v, int *out) {
    if (!strcasecmp(v, "center") || !strcmp(v, "1") || !strcmp(v, "16:9")) *out = UI_CENTER;
    else if (!strcmp(v, "2") || !strcmp(v, "21:9")) *out = UI_21_9;
    else if (!strcmp(v, "3") || !strcmp(v, "32:9")) *out = UI_32_9;
    else if (!strcasecmp(v, "edges")) *out = UI_EDGES;
    else return false;
    return true;
}

// FSRQuality (squashed): NativeAA, Quality, Balanced, Performance, UltraPerformance, as Config.fsr_quality's FSR_*
static bool parse_fsr_quality(const char *v, int *out) {
    if (!strcmp(v, "nativeaa") || !strcmp(v, "native") || !strcmp(v, "aa")) *out = FSR_NATIVE;
    else if (!strcmp(v, "quality")) *out = FSR_QUALITY;
    else if (!strcmp(v, "balanced")) *out = FSR_BALANCED;
    else if (!strcmp(v, "performance")) *out = FSR_PERFORMANCE;
    else if (!strcmp(v, "ultraperformance") || !strcmp(v, "ultraperf")) *out = FSR_ULTRA_PERFORMANCE;
    else return false;
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
    } else if (!strcmp(k, "aspectratio")) {
        ok = parse_aspect(v, &g_config.aspect_ratio);
    } else if (!strcmp(k, "fov")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.5f && f <= 2.0f)) g_config.fov = f;
    } else if (!strcmp(k, "uianchor")) {
        ok = parse_anchor(v, &g_config.ui_anchor);
    } else if (!strcmp(k, "msaa")) {
        if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "0") || !strcmp(v, "1") || !strcmp(v, "1x"))
            g_config.msaa = 1;
        else if (!strcmp(v, "2x") || !strcmp(v, "2")) g_config.msaa = 2;
        else if (!strcmp(v, "4x") || !strcmp(v, "4")) g_config.msaa = 4;
        else if (!strcmp(v, "8x") || !strcmp(v, "8")) g_config.msaa = 8;
        else ok = false;
    } else if (!strcmp(k, "backgroundinput")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.background_input = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.background_input = 0;
        else ok = false;
    } else if (!strcmp(k, "fxaa")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fxaa = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fxaa = 0;
        else ok = false;
    } else if (!strcmp(k, "fsr")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fsr = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fsr = 0;
        else ok = false;
    } else if (!strcmp(k, "upscaler")) {
        if (!strcmp(v, "fsr3") || !strcmp(v, "fsr3.1") || !strcmp(v, "3")) g_config.fsr_upscaler = FSR_UP_3;
        else if (!strcmp(v, "fsr4") || !strcmp(v, "4")) g_config.fsr_upscaler = FSR_UP_4;
        else ok = false;
    } else if (!strcmp(k, "fsrquality")) {
        ok = parse_fsr_quality(v, &g_config.fsr_quality);
    } else if (!strcmp(k, "sharpness")) {
        char *end;
        float f = strtof(v, &end);
        if ((ok = end != v && f >= 0.0f && f <= 1.0f)) g_config.fsr_sharpness = f;
    } else if (!strcmp(k, "objectmotion")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fsr_objmotion = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fsr_objmotion = 0;
        else ok = false;
    } else if (!strcmp(k, "animsurf")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fsr_animsurf = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fsr_animsurf = 0;
        else ok = false;
    } else if (!strcmp(k, "reactive")) {
        char *end;
        float f = strtof(v, &end);
        if ((ok = end != v && f >= 0.0f && f <= 1.0f)) g_config.fsr_reactive = f;
    } else if (!strcmp(k, "vsync")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.vsync = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.vsync = 0;
        else ok = false;
    } else if (!strcmp(k, "facebuttons")) {
        if (!strcmp(v, "position") || !strcmp(v, "positional") || !strcmp(v, "nintendo"))
            g_config.face_buttons = FACE_POSITION;
        else if (!strcmp(v, "label") || !strcmp(v, "labels") || !strcmp(v, "xbox"))
            g_config.face_buttons = FACE_LABEL;
        else ok = false;
    } else if (!strcmp(k, "mousesensitivity")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.1f && f <= 10.0f)) g_config.mouse_sensitivity = f;
    } else if (!strcmp(k, "port")) {
        int p = atoi(v);
        if (p > 0 && p < 65536) g_config.ldn_port = p;
        else ok = false;
    } else if (!strcmp(k, "join")) {
        snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", val);  // (as written: names keep their dashes)
    } else if (!strcmp(k, "bonuscontent")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.bonus_content = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.bonus_content = 0;
        else ok = false;
    } else if (!strcmp(k, "villagequestsinhub")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.village_in_hub = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.village_in_hub = 0;
        else ok = false;
    } else if (!strcmp(k, "hubhp")) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.village = f[0];
    } else if (!strcmp(k, "syncsmallmonsters")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.sync_small_monsters = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.sync_small_monsters = 0;
        else ok = false;
    } else if (k[0] >= '1' && k[0] <= '4' && (!strcmp(k + 1, "player") || !strcmp(k + 1, "players"))) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.scale[k[0] - '1'] = f[0];
    } else if (!strncmp(k, "quest", 5) && isdigit((unsigned char)k[5])) {
        HpScale *h = &g_config.hp;
        char *end;
        unsigned long q = strtoul(k + 5, &end, 10);
        float f[4];
        int n = parse_factors(v, f);
        if ((ok = !*end && q && (n == 1 || n == 4))) {
            int i = 0;
            while (i < h->quests && h->quest[i].quest != q) i++;
            if (i == HP_QUESTS_MAX) { LOG("[config] line %d: more than %d quests set apart\n", line, HP_QUESTS_MAX); return; }
            h->quest[i].quest = (uint32_t)q;
            for (int j = 0; j < 4; j++) h->quest[i].scale[j] = f[n == 4 ? j : 0];
            if (i == h->quests) h->quests++;
        }
    } else if (!strcmp(k, "fps") || !strcmp(k, "framerate")) {
        int f = atoi(v);
        if (f >= 30 && f <= 240) g_config.fps = f;
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

// The largest size inside w x h matching the given aspect ratio, even numbers.
static void fit_aspect(int w, int h, float aspect, int *rw, int *rh) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if ((float)w / (float)h > aspect)
        w = (int)((float)h * aspect + 0.5f);
    else
        h = (int)((float)w / aspect + 0.5f);
    *rw = w & ~1, *rh = h & ~1;
}

// Sections of DEFAULT_INI the file lacks (written by an older build) are added with their defaults at its end; the
// player's own settings stay as they are.
static void append_missing_sections(const char *path, const char *seen) {
    FILE *o = NULL;
    for (const char *p = strstr(DEFAULT_INI, "\n["); p;) {
        const char *start = p + 1, *end = strstr(start, "\n["), *close = strchr(start, ']');
        p = end;
        char name[64], key[70];
        snprintf(name, sizeof name, "%.*s", (int)(close - start - 1), start + 1);
        for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
        snprintf(key, sizeof key, "|%s|", name);
        if (strstr(seen, key)) continue;
        if (!o && !(o = fopen(path, "a"))) return;
        fprintf(o, "\n%.*s", (int)(end ? end - start : (long)strlen(start)), start);
        LOG("[config] added the [%s] section to %s\n", name, path);
    }
    if (o) fclose(o);
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
        char buf[512], seen[512] = "|";
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (*s == '[' && strchr(s, ']') && strlen(seen) + strlen(s) + 2 < sizeof seen) {
                for (char *q = s + 1; *q != ']'; q++) {
                    size_t n = strlen(seen);
                    seen[n] = (char)tolower((unsigned char)*q), seen[n + 1] = 0;
                }
                strcat(seen, "|");
            }
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
        append_missing_sections(path, seen);
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
    if ((e = getenv("MHGU_BACKGROUND_INPUT"))) g_config.background_input = atoi(e) != 0;
    if ((e = getenv("MHGU_PAD_POSITIONAL"))) g_config.face_buttons = atoi(e) ? FACE_POSITION : FACE_LABEL;
    if ((e = getenv("MHGU_LDN_PORT")) && atoi(e) > 0) g_config.ldn_port = atoi(e);
    if ((e = getenv("MHGU_SYNC_ZAKO"))) g_config.sync_small_monsters = atoi(e) != 0;
    if ((e = getenv("MHGU_LDN_JOIN"))) snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", e);
    if ((e = getenv("MHGU_ASPECT"))) parse_aspect(e, &g_config.aspect_ratio);
    if ((e = getenv("MHGU_FOV")) && strtof(e, NULL) >= 0.5f && strtof(e, NULL) <= 2.0f) g_config.fov = strtof(e, NULL);
    if ((e = getenv("MHGU_UI_ANCHOR"))) parse_anchor(e, &g_config.ui_anchor);
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;
    if ((e = getenv("MHGU_FSR"))) g_config.fsr = atoi(e) != 0;  // (test runs: MHGU_FSR=0|1, MHGU_FSR_QUALITY=quality...)
    if ((e = getenv("MHGU_FSR_UPSCALER"))) g_config.fsr_upscaler = !strcmp(e, "4") || !strcasecmp(e, "fsr4") ? FSR_UP_4 : FSR_UP_3;
    if ((e = getenv("MHGU_FSR_QUALITY"))) {
        char q[32];
        snprintf(q, sizeof q, "%s", e);
        squash(q);
        parse_fsr_quality(q, &g_config.fsr_quality);
    }
    if ((e = getenv("MHGU_FSR_OBJMOTION"))) g_config.fsr_objmotion = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_ANIMSURF"))) g_config.fsr_animsurf = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_SHARPNESS")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_sharpness = strtof(e, NULL);
    if ((e = getenv("MHGU_FSR_REACTIVE")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_reactive = strtof(e, NULL);

    int tw = g_config.width, th = g_config.height;  // (Borderless shows the image at the desktop size)
    if (g_config.display_mode == DISPLAY_BORDERLESS) tw = tw < desktop_w ? tw : desktop_w, th = th < desktop_h ? th : desktop_h;
    // The picture's shape: the window's or AspectRatio's, at least 16:10. The game lays its HUD out for 16:9, which
    // still fits down to 16:10 (a little flatter, nvn/vulkan/exec.c), so a narrower window (4:3) shows 16:10 with bars.
    // Wider than 16:9 shows more to the sides, 16:9 to 16:10 more above and below (camera_fovy, gen.py PATCHES).
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : (float)tw / (float)th;
    if (aspect < 1.6f - 0.005f) {
        if (g_config.aspect_ratio > 0)
            LOG("[config] AspectRatio %.3f is narrower than 16:10, which the HUD needs: using 16:10\n", aspect);
        aspect = 1.6f;
    }
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;  // (1366x768 and the like)
    if (render_auto && g_config.fsr && g_config.display_mode == DISPLAY_BORDERLESS && (tw < desktop_w || th < desktop_h)) {
        // FSR, Borderless: it upscales straight to the size the picture is shown at (the screen, in the picture's shape)
        // instead of to Resolution, which the window would then stretch again (blurring what FSR rebuilt)
        fit_aspect(desktop_w, desktop_h, aspect, &w, &h);
        LOG("[config] FSR, Borderless: upscaling to %dx%d (the screen, in the picture's shape) instead of Resolution %dx%d\n",
            w, h, tw, th);
    } else if (render_auto) {
        fit_aspect(tw, th, aspect, &w, &h);
    } else {
        fit_aspect(g_config.render_w, g_config.render_h, aspect, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not the picture's shape (%.3f): using %dx%d\n", g_config.render_w,
                g_config.render_h, aspect, w, h);
    }
    if (g_config.fsr) {  // FSR: that size is its output; the game renders smaller (runtime/nvn/vulkan/fsr.c)
        static const float ratio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        static const char *const names[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
        g_fsr_output[0] = (uint32_t)w, g_fsr_output[1] = (uint32_t)h;
        float r = ratio[g_config.fsr_quality];
        fit_aspect((int)(w / r + 0.5f), (int)(h / r + 0.5f), aspect, &w, &h);
        if (g_config.msaa > 1) LOG("[config] FSR is on: MSAA off (FSR does the anti-aliasing)\n");
        if (g_config.fxaa) LOG("[config] FSR is on: FXAA off (FSR does the anti-aliasing)\n");
        g_config.msaa = 1, g_config.fxaa = 0;
        // textures are sampled for the output size (AMD's recommendation: log2(render / output)); MHGU_FSR_LODBIAS
        // overrides (A/B)
        g_fsr_lod_bias = log2f((float)w / (float)g_fsr_output[0]);
        if ((e = getenv("MHGU_FSR_LODBIAS"))) g_fsr_lod_bias = strtof(e, NULL);
        LOG("[config] FSR %s %s (x%.1f): the 3D picture at %dx%d, upscaled to %ux%u, sharpness %.2f, texture LOD bias "
            "%.2f\n", g_config.fsr_upscaler == FSR_UP_4 ? "4 (3.1 if it cannot start)" : "3.1", names[g_config.fsr_quality], r, w, h, g_fsr_output[0], g_fsr_output[1], g_config.fsr_sharpness,
            g_fsr_lod_bias);
    }
    g_config.render_w = w, g_config.render_h = h;
    g_render_size[0] = (uint32_t)w;
    g_render_size[1] = (uint32_t)h;
    g_render_aspect = aspect;
    g_fov_scale = g_config.fov;

    // The HUD's width: 16:9 (Center), 21:9, 32:9 or the picture's (Edges), at most the picture's; the game's 2D canvas
    // gets that width (gen.py PATCHES), exec.c places the HUD's parts in it.
    float ui = g_config.ui_anchor == UI_EDGES  ? aspect
             : g_config.ui_anchor == UI_32_9 ? 32.0f / 9.0f
             : g_config.ui_anchor == UI_21_9 ? 21.0f / 9.0f
                                             : 16.0f / 9.0f;
    g_ui_anchor_aspect = ui < aspect ? ui : aspect;
    g_ui_size[1] = 720;
    g_ui_size[0] = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
    if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;

    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d (shape %.3f, FOV x%.2f, HUD %.3f wide), %d fps, vsync %s, MSAA %dx, FXAA %s, "
        "face buttons by %s, controller %s in the background\n", path, modes[g_config.display_mode], g_config.width,
        g_config.height, g_config.render_w, g_config.render_h, g_render_aspect, g_fov_scale, g_ui_anchor_aspect,
        g_config.fps, g_config.vsync ? "on" : "off",
        g_config.msaa, g_config.fxaa ? "on" : "off", g_config.face_buttons == FACE_LABEL ? "label" : "position",
        g_config.background_input ? "on" : "off");
    const HpScale *hp = &g_config.hp;
    LOG("[config] monster HP in Hub quests: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, %d quest(s) set apart\n",
        hp->scale[0], hp->scale[1], hp->scale[2], hp->scale[3], hp->quests);
    LOG("[config] Village quests at the Hub counter: %s (their HP x%.2f first)\n", g_config.village_in_hub ? "on" : "off",
        hp->village);
}

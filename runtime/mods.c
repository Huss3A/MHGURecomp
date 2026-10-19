// Mod folders (mods.h).
#include "mods.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"

static const char *mods_dir(void) {
    const char *e = getenv("MHGU_MODS_DIR");
    return e && *e ? e : "mods";
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_mods(char **names, int max) {
    int n = 0;
    char path[1100];
    snprintf(path, sizeof path, "%s/load_order.txt", mods_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (n < max && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            names[n++] = strdup(s);
        }
        fclose(f);
        return n;
    }
    DIR *d = opendir(mods_dir());
    if (!d) return 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        snprintf(path, sizeof path, "%s/%s", mods_dir(), e->d_name);
        struct stat st;
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp);
    return n;
}

int mods_romfs_layers(const char **out, int max) {
    char *names[256];
    int n = list_mods(names, 256), k = 0;
    for (int i = 0; i < n; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s/romfs", mods_dir(), names[i]);
        struct stat st;
        if (k < max && !stat(path, &st) && S_ISDIR(st.st_mode)) {
            out[k++] = strdup(path);
            LOG("[mods] %s\n", names[i]);
        }
        free(names[i]);
    }
    return k;
}

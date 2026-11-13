#include "fs_paths_ps2.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char asset_root[CS_PS2_ROOT_BYTES] = "host:";
static char launch_path[CS_PS2_ROOT_BYTES];
static int resolved;

static int safe_segments(const char *path)
{
    const char *p;
    for (p = path; *p; ++p) {
        if (*p == ':') return 0;
        if (p[0] == '.' && p[1] == '.' &&
            (p == path || p[-1] == '/') && (p[2] == '/' || !p[2])) return 0;
    }
    return 1;
}
static int normalize_root(const char *root, char out[CS_PS2_ROOT_BYTES])
{
    char *colon;
    size_t i, length;
    if (!root || !(length = strlen(root)) || length >= CS_PS2_ROOT_BYTES) return 0;
    memcpy(out, root, length + 1);
    for (i = 0; i < length; ++i) if (out[i] == '\\') out[i] = '/';
    colon = strchr(out, ':');
    if (!colon || colon - out < 2 || colon - out > 15) return 0;
    for (i = 0; out + i < colon; ++i)
        if (!isalnum((unsigned char)out[i]) && out[i] != '_') return 0;
    if (!safe_segments(colon + 1)) return 0;
    /* Some loaders pass argv[0] as "mass:CS16/x.elf". usbhdfsd opens such
     * relative paths but hangs in dopen, so mass roots always get "/". */
    if (strncmp(out, "host", 4) && colon[1] && colon[1] != '/') {
        if (length + 1 >= CS_PS2_ROOT_BYTES) return 0;
        memmove(colon + 2, colon + 1, length - (size_t)(colon - out));
        colon[1] = '/';
        ++length;
    }
    while (length > (size_t)(colon - out) + 2 && out[length - 1] == '/')
        out[--length] = 0;
    return 1;
}
int PS2_FS_SetRoot(const char *root)
{
    char normalized[CS_PS2_ROOT_BYTES];
    if (!normalize_root(root, normalized)) return 0;
    strcpy(asset_root, normalized);
    resolved = 1;
    return 1;
}
void PS2_FS_ResetRoot(void)
{
    strcpy(asset_root, "host:");
    resolved = 0;
}
const char *PS2_FS_Root(void) { return asset_root; }
int PS2_FS_RootResolved(void) { return resolved; }
void PS2_FS_SetLaunchPath(const char *path)
{
    /* Retain the original launcher argument across game changes. */
    if (!launch_path[0] && path && strlen(path) < sizeof(launch_path))
        strcpy(launch_path, path);
}
const char *PS2_FS_LaunchPath(void) { return launch_path; }

int PS2_FS_JoinRoot(const char *root, const char *source,
                    char *destination, size_t capacity)
{
    char base[CS_PS2_ROOT_BYTES], relative[1024];
    const char *p;
    size_t i, length, root_length;
    int n;
    if (!source || !destination || !capacity || !normalize_root(root, base)) return 0;
    length = strlen(source);
    if (length >= sizeof(relative)) return 0;
    memcpy(relative, source, length + 1);
    for (i = 0; i < length; ++i) if (relative[i] == '\\') relative[i] = '/';
    p = relative;
    root_length = strlen(base);
    if (strchr(p, ':')) {
        /* Native paths are accepted only beneath the selected installation. */
        if (strncmp(p, base, root_length)) return 0;
        if (base[root_length - 1] != ':' && base[root_length - 1] != '/' &&
            p[root_length] && p[root_length] != '/') return 0;
        p += root_length;
        while (*p == '/') ++p;
    } else if (*p == '/') return 0;
    while (p[0] == '.' && p[1] == '/') p += 2;
    if (!safe_segments(p)) return 0;
    if (!*p || !strcmp(p, ".")) {
        n = snprintf(destination, capacity, "%s%s", base,
                     base[root_length - 1] == ':' ? "." : "");
    } else {
        n = snprintf(destination, capacity, "%s%s%s", base,
                     base[root_length - 1] == ':' || base[root_length - 1] == '/' ? "" : "/", p);
    }
    return n >= 0 && (size_t)n < capacity;
}
int PS2_FS_MakePath(const char *source, char *destination, size_t capacity)
{
    return PS2_FS_JoinRoot(asset_root, source, destination, capacity);
}
int PS2_FS_MakeHostPath(const char *source, char *destination, size_t capacity)
{
    return PS2_FS_MakePath(source, destination, capacity);
}
